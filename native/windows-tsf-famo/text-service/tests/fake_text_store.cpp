#include "fake_text_store.h"

#include <algorithm>
#include <cstring>

#include <olectl.h>

namespace famo::tsf::test {

namespace {

constexpr TsViewCookie kViewCookie = 1;

bool SameUnknown(IUnknown *left, IUnknown *right) {
  if (!left || !right)
    return false;
  IUnknown *left_identity = nullptr;
  IUnknown *right_identity = nullptr;
  const HRESULT left_result =
      left->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&left_identity));
  const HRESULT right_result = right->QueryInterface(
      IID_IUnknown, reinterpret_cast<void **>(&right_identity));
  const bool same = SUCCEEDED(left_result) && SUCCEEDED(right_result) &&
                    left_identity == right_identity;
  if (left_identity)
    left_identity->Release();
  if (right_identity)
    right_identity->Release();
  return same;
}

} // namespace

FakeTextStore::FakeTextStore() {
  selection_.acpStart = 0;
  selection_.acpEnd = 0;
  selection_.style.ase = TS_AE_END;
  selection_.style.fInterimChar = FALSE;
}

FakeTextStore::~FakeTextStore() {
  if (sink_)
    sink_->Release();
}

HRESULT FakeTextStore::QueryInterface(REFIID iid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (iid == IID_IUnknown || iid == IID_ITextStoreACP)
    *object = static_cast<ITextStoreACP *>(this);
  if (!*object)
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

ULONG FakeTextStore::AddRef() { return ++references_; }

ULONG FakeTextStore::Release() {
  const ULONG remaining = --references_;
  if (remaining == 0)
    delete this;
  return remaining;
}

HRESULT FakeTextStore::AdviseSink(REFIID iid, IUnknown *unknown, DWORD mask) {
  if (iid != IID_ITextStoreACPSink || !unknown)
    return E_INVALIDARG;
  ITextStoreACPSink *sink = nullptr;
  const HRESULT result = unknown->QueryInterface(
      IID_ITextStoreACPSink, reinterpret_cast<void **>(&sink));
  if (FAILED(result))
    return result;
  if (sink_) {
    if (!SameUnknown(sink_, sink)) {
      sink->Release();
      return CONNECT_E_ADVISELIMIT;
    }
    sink_->Release();
  }
  sink_ = sink;
  sink_mask_ = mask;
  return S_OK;
}

HRESULT FakeTextStore::UnadviseSink(IUnknown *unknown) {
  if (!sink_ || !SameUnknown(sink_, unknown))
    return CONNECT_E_NOCONNECTION;
  sink_->Release();
  sink_ = nullptr;
  sink_mask_ = 0;
  return S_OK;
}

HRESULT FakeTextStore::RequestLock(DWORD flags, HRESULT *session_result) {
  ++lock_request_count_;
  if (!session_result)
    return E_POINTER;
  if (deny_locks_) {
    *session_result = E_FAIL;
    return E_FAIL;
  }
  if (!sink_) {
    *session_result = E_UNEXPECTED;
    return E_UNEXPECTED;
  }
  if (lock_flags_ != 0) {
    *session_result = TS_E_SYNCHRONOUS;
    return S_OK;
  }
  lock_flags_ = flags;
  *session_result = sink_->OnLockGranted(flags);
  lock_flags_ = 0;
  return S_OK;
}

HRESULT FakeTextStore::GetStatus(TS_STATUS *status) {
  if (!status)
    return E_POINTER;
  status->dwDynamicFlags = 0;
  status->dwStaticFlags = TS_SS_NOHIDDENTEXT;
  return S_OK;
}

HRESULT FakeTextStore::QueryInsert(LONG test_start, LONG test_end, ULONG count,
                                   LONG *result_start, LONG *result_end) {
  if (!result_start || !result_end)
    return E_POINTER;
  if (!ValidRange(test_start, test_end))
    return TS_E_INVALIDPOS;
  *result_start = test_start;
  *result_end = test_start + static_cast<LONG>(count);
  return S_OK;
}

HRESULT FakeTextStore::GetSelection(ULONG index, ULONG count,
                                    TS_SELECTION_ACP *selection,
                                    ULONG *fetched) {
  if (!fetched || (count != 0 && !selection))
    return E_POINTER;
  if (!HasReadLock())
    return TS_E_NOLOCK;
  *fetched = 0;
  if (count == 0)
    return S_OK;
  if (index != TS_DEFAULT_SELECTION && index != 0)
    return E_INVALIDARG;
  selection[0] = selection_;
  *fetched = 1;
  return S_OK;
}

HRESULT FakeTextStore::SetSelection(ULONG count,
                                    const TS_SELECTION_ACP *selection) {
  if (!selection)
    return E_POINTER;
  if (!HasWriteLock())
    return TS_E_NOLOCK;
  if (count != 1 || !ValidRange(selection[0].acpStart, selection[0].acpEnd))
    return TS_E_INVALIDPOS;
  selection_ = selection[0];
  return S_OK;
}

HRESULT FakeTextStore::GetText(LONG start, LONG end, WCHAR *plain,
                               ULONG plain_capacity, ULONG *plain_count,
                               TS_RUNINFO *runs, ULONG run_capacity,
                               ULONG *run_count, LONG *next) {
  if (!plain_count || !run_count || !next)
    return E_POINTER;
  if (!HasReadLock())
    return TS_E_NOLOCK;
  if (end == -1)
    end = static_cast<LONG>(text_.size());
  if (!ValidRange(start, end))
    return TS_E_INVALIDPOS;
  const ULONG available = static_cast<ULONG>(end - start);
  const ULONG copied = std::min(available, plain_capacity);
  if (copied != 0 && !plain)
    return E_POINTER;
  if (copied != 0)
    std::copy_n(text_.data() + start, copied, plain);
  *plain_count = copied;
  *run_count = 0;
  if (run_capacity != 0) {
    if (!runs)
      return E_POINTER;
    runs[0].uCount = copied;
    runs[0].type = TS_RT_PLAIN;
    *run_count = copied == 0 ? 0 : 1;
  }
  *next = start + static_cast<LONG>(copied);
  return S_OK;
}

HRESULT FakeTextStore::SetText(DWORD, LONG start, LONG end,
                               const WCHAR *text, ULONG count,
                               TS_TEXTCHANGE *change) {
  if (!HasWriteLock())
    return TS_E_NOLOCK;
  return Replace(start, end, text, count, change);
}

HRESULT FakeTextStore::GetFormattedText(LONG, LONG, IDataObject **data) {
  if (!data)
    return E_POINTER;
  *data = nullptr;
  return E_NOTIMPL;
}

HRESULT FakeTextStore::GetEmbedded(LONG, REFGUID, REFIID,
                                   IUnknown **unknown) {
  if (!unknown)
    return E_POINTER;
  *unknown = nullptr;
  return TS_E_NOOBJECT;
}

HRESULT FakeTextStore::QueryInsertEmbedded(const GUID *, const FORMATETC *,
                                           BOOL *insertable) {
  if (!insertable)
    return E_POINTER;
  *insertable = FALSE;
  return S_OK;
}

HRESULT FakeTextStore::InsertEmbedded(DWORD, LONG, LONG, IDataObject *,
                                      TS_TEXTCHANGE *) {
  return TS_E_FORMAT;
}

HRESULT FakeTextStore::InsertTextAtSelection(
    DWORD flags, const WCHAR *text, ULONG count, LONG *start, LONG *end,
    TS_TEXTCHANGE *change) {
  if (!start || !end || !change)
    return E_POINTER;
  if (!HasWriteLock())
    return TS_E_NOLOCK;
  const LONG old_start = selection_.acpStart;
  const LONG old_end = selection_.acpEnd;
  *start = old_start;
  *end = old_start + static_cast<LONG>(count);
  change->acpStart = old_start;
  change->acpOldEnd = old_end;
  change->acpNewEnd = *end;
  if ((flags & TS_IAS_QUERYONLY) != 0)
    return S_OK;
  const HRESULT result = Replace(old_start, old_end, text, count, change);
  if (SUCCEEDED(result)) {
    selection_.acpStart = *end;
    selection_.acpEnd = *end;
  }
  return result;
}

HRESULT FakeTextStore::InsertEmbeddedAtSelection(DWORD, IDataObject *, LONG *,
                                                 LONG *, TS_TEXTCHANGE *) {
  return TS_E_FORMAT;
}

HRESULT FakeTextStore::RequestSupportedAttrs(DWORD, ULONG,
                                              const TS_ATTRID *) {
  return S_OK;
}

HRESULT FakeTextStore::RequestAttrsAtPosition(LONG, ULONG,
                                               const TS_ATTRID *, DWORD) {
  return S_OK;
}

HRESULT FakeTextStore::RequestAttrsTransitioningAtPosition(
    LONG, ULONG, const TS_ATTRID *, DWORD) {
  return S_OK;
}

HRESULT FakeTextStore::FindNextAttrTransition(
    LONG, LONG halt, ULONG, const TS_ATTRID *, DWORD, LONG *next, BOOL *found,
    LONG *found_offset) {
  if (!next || !found || !found_offset)
    return E_POINTER;
  *next = halt;
  *found = FALSE;
  *found_offset = 0;
  return S_OK;
}

HRESULT FakeTextStore::RetrieveRequestedAttrs(ULONG, TS_ATTRVAL *,
                                              ULONG *fetched) {
  if (!fetched)
    return E_POINTER;
  *fetched = 0;
  return S_OK;
}

HRESULT FakeTextStore::GetEndACP(LONG *end) {
  if (!end)
    return E_POINTER;
  if (!HasReadLock())
    return TS_E_NOLOCK;
  *end = static_cast<LONG>(text_.size());
  return S_OK;
}

HRESULT FakeTextStore::GetActiveView(TsViewCookie *view) {
  if (!view)
    return E_POINTER;
  *view = kViewCookie;
  return S_OK;
}

HRESULT FakeTextStore::GetACPFromPoint(TsViewCookie, const POINT *, DWORD,
                                       LONG *position) {
  if (!position)
    return E_POINTER;
  *position = selection_.acpEnd;
  return S_OK;
}

HRESULT FakeTextStore::GetTextExt(TsViewCookie, LONG, LONG, RECT *rect,
                                  BOOL *clipped) {
  if (!rect || !clipped)
    return E_POINTER;
  *rect = {0, 0, 1, 16};
  *clipped = FALSE;
  return S_OK;
}

HRESULT FakeTextStore::GetScreenExt(TsViewCookie, RECT *rect) {
  if (!rect)
    return E_POINTER;
  *rect = {0, 0, 1024, 768};
  return S_OK;
}

HRESULT FakeTextStore::GetWnd(TsViewCookie, HWND *window) {
  if (!window)
    return E_POINTER;
  *window = nullptr;
  return S_OK;
}

bool FakeTextStore::HasReadLock() const {
  return (lock_flags_ & (TS_LF_READ | TS_LF_READWRITE)) != 0;
}

bool FakeTextStore::HasWriteLock() const {
  return (lock_flags_ & TS_LF_READWRITE) != 0;
}

bool FakeTextStore::ValidRange(LONG start, LONG end) const {
  return start >= 0 && end >= start &&
         end <= static_cast<LONG>(text_.size());
}

HRESULT FakeTextStore::Replace(LONG start, LONG end, const WCHAR *text,
                               ULONG count, TS_TEXTCHANGE *change) {
  if (!change || (count != 0 && !text))
    return E_POINTER;
  if (!ValidRange(start, end))
    return TS_E_INVALIDPOS;
  ++replace_count_;
  text_.replace(static_cast<size_t>(start), static_cast<size_t>(end - start),
                text ? text : L"", count);
  change->acpStart = start;
  change->acpOldEnd = end;
  change->acpNewEnd = start + static_cast<LONG>(count);
  return S_OK;
}

} // namespace famo::tsf::test
