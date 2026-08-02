#pragma once

#include <atomic>
#include <cstddef>
#include <string>

#include <textstor.h>

namespace famo::tsf::test {

class FakeTextStore final : public ITextStoreACP {
public:
  FakeTextStore();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown *unknown,
                                       DWORD mask) override;
  HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown *unknown) override;
  HRESULT STDMETHODCALLTYPE RequestLock(DWORD flags,
                                        HRESULT *session_result) override;
  HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS *status) override;
  HRESULT STDMETHODCALLTYPE QueryInsert(LONG test_start, LONG test_end,
                                        ULONG count, LONG *result_start,
                                        LONG *result_end) override;
  HRESULT STDMETHODCALLTYPE GetSelection(ULONG index, ULONG count,
                                         TS_SELECTION_ACP *selection,
                                         ULONG *fetched) override;
  HRESULT STDMETHODCALLTYPE SetSelection(
      ULONG count, const TS_SELECTION_ACP *selection) override;
  HRESULT STDMETHODCALLTYPE GetText(LONG start, LONG end, WCHAR *plain,
                                    ULONG plain_capacity,
                                    ULONG *plain_count, TS_RUNINFO *runs,
                                    ULONG run_capacity, ULONG *run_count,
                                    LONG *next) override;
  HRESULT STDMETHODCALLTYPE SetText(DWORD flags, LONG start, LONG end,
                                    const WCHAR *text, ULONG count,
                                    TS_TEXTCHANGE *change) override;
  HRESULT STDMETHODCALLTYPE GetFormattedText(LONG start, LONG end,
                                             IDataObject **data) override;
  HRESULT STDMETHODCALLTYPE GetEmbedded(LONG position, REFGUID service,
                                        REFIID iid,
                                        IUnknown **unknown) override;
  HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(
      const GUID *service, const FORMATETC *format,
      BOOL *insertable) override;
  HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD flags, LONG start, LONG end,
                                           IDataObject *data,
                                           TS_TEXTCHANGE *change) override;
  HRESULT STDMETHODCALLTYPE InsertTextAtSelection(
      DWORD flags, const WCHAR *text, ULONG count, LONG *start, LONG *end,
      TS_TEXTCHANGE *change) override;
  HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(
      DWORD flags, IDataObject *data, LONG *start, LONG *end,
      TS_TEXTCHANGE *change) override;
  HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(
      DWORD flags, ULONG count, const TS_ATTRID *attributes) override;
  HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(
      LONG position, ULONG count, const TS_ATTRID *attributes,
      DWORD flags) override;
  HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(
      LONG position, ULONG count, const TS_ATTRID *attributes,
      DWORD flags) override;
  HRESULT STDMETHODCALLTYPE FindNextAttrTransition(
      LONG start, LONG halt, ULONG count, const TS_ATTRID *attributes,
      DWORD flags, LONG *next, BOOL *found, LONG *found_offset) override;
  HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG count,
                                                   TS_ATTRVAL *values,
                                                   ULONG *fetched) override;
  HRESULT STDMETHODCALLTYPE GetEndACP(LONG *end) override;
  HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie *view) override;
  HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie view,
                                            const POINT *point, DWORD flags,
                                            LONG *position) override;
  HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie view, LONG start, LONG end,
                                       RECT *rect, BOOL *clipped) override;
  HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie view,
                                         RECT *rect) override;
  HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie view, HWND *window) override;

  const std::wstring &text() const { return text_; }
  const TS_SELECTION_ACP &selection() const { return selection_; }
  size_t replace_count() const { return replace_count_; }
  size_t lock_request_count() const { return lock_request_count_; }
  size_t text_ext_query_count() const { return text_ext_query_count_; }
  size_t screen_ext_query_count() const { return screen_ext_query_count_; }
  size_t window_query_count() const { return window_query_count_; }
  LONG last_text_ext_start() const { return last_text_ext_start_; }
  LONG last_text_ext_end() const { return last_text_ext_end_; }
  void set_deny_locks(bool deny) { deny_locks_ = deny; }
  void set_window_for_test(HWND window) { window_ = window; }
  void set_selection_for_test(LONG start, LONG end, TsActiveSelEnd active_end) {
    selection_.acpStart = start;
    selection_.acpEnd = end;
    selection_.style.ase = active_end;
    selection_.style.fInterimChar = FALSE;
  }

private:
  ~FakeTextStore();
  bool HasReadLock() const;
  bool HasWriteLock() const;
  bool ValidRange(LONG start, LONG end) const;
  HRESULT Replace(LONG start, LONG end, const WCHAR *text, ULONG count,
                  TS_TEXTCHANGE *change);

  std::atomic<ULONG> references_{1};
  ITextStoreACPSink *sink_ = nullptr;
  DWORD sink_mask_ = 0;
  DWORD lock_flags_ = 0;
  bool deny_locks_ = false;
  size_t lock_request_count_ = 0;
  size_t replace_count_ = 0;
  size_t text_ext_query_count_ = 0;
  size_t screen_ext_query_count_ = 0;
  size_t window_query_count_ = 0;
  LONG last_text_ext_start_ = 0;
  LONG last_text_ext_end_ = 0;
  HWND window_ = nullptr;
  std::wstring text_;
  TS_SELECTION_ACP selection_{};
};

} // namespace famo::tsf::test
