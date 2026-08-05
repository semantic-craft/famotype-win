#include "display_attribute.h"

#include <atomic>
#include <new>

#include <oleauto.h>

#include "famo_guids.h"
#include "module_state.h"

namespace famo::tsf {
namespace {

// A solid thin underline in the host's own colors. Leaving every colour as
// TF_CT_NONE lets the host pick contrast that matches its theme, which is what
// a self-drawn candidate window wants: the host owns the inline run, Famo owns
// the popup.
TF_DISPLAYATTRIBUTE CompositionAttribute() noexcept {
  TF_DISPLAYATTRIBUTE attribute{};
  attribute.crText.type = TF_CT_NONE;
  attribute.crBk.type = TF_CT_NONE;
  attribute.lsStyle = TF_LS_SOLID;
  attribute.fBoldLine = FALSE;
  attribute.crLine.type = TF_CT_NONE;
  attribute.bAttr = TF_ATTR_INPUT;
  return attribute;
}

class DisplayAttributeInfo final : public ITfDisplayAttributeInfo {
public:
  DisplayAttributeInfo() : attribute_(CompositionAttribute()) {
    AddModuleObject();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfDisplayAttributeInfo)
      *object = static_cast<ITfDisplayAttributeInfo *>(this);
    if (!*object)
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0)
      delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE GetGUID(GUID *guid) override {
    if (!guid)
      return E_POINTER;
    *guid = kDisplayAttributeGuid;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDescription(BSTR *description) override {
    if (!description)
      return E_POINTER;
    *description = SysAllocString(L"Famo composition");
    return *description ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT STDMETHODCALLTYPE
  GetAttributeInfo(TF_DISPLAYATTRIBUTE *attribute) override {
    if (!attribute)
      return E_POINTER;
    *attribute = attribute_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  SetAttributeInfo(const TF_DISPLAYATTRIBUTE *attribute) override {
    if (!attribute)
      return E_POINTER;
    attribute_ = *attribute;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Reset() override {
    attribute_ = CompositionAttribute();
    return S_OK;
  }

private:
  ~DisplayAttributeInfo() { RemoveModuleObject(); }

  std::atomic<ULONG> references_{1};
  TF_DISPLAYATTRIBUTE attribute_;
};

// Famo publishes exactly one attribute, so the enumerator is a one-item
// cursor rather than a container.
class DisplayAttributeInfoEnum final : public IEnumTfDisplayAttributeInfo {
public:
  explicit DisplayAttributeInfoEnum(bool consumed = false)
      : consumed_(consumed) {
    AddModuleObject();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IEnumTfDisplayAttributeInfo)
      *object = static_cast<IEnumTfDisplayAttributeInfo *>(this);
    if (!*object)
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0)
      delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE
  Clone(IEnumTfDisplayAttributeInfo **enumerator) override {
    if (!enumerator)
      return E_POINTER;
    *enumerator = nullptr;
    auto *clone = new (std::nothrow) DisplayAttributeInfoEnum(consumed_);
    if (!clone)
      return E_OUTOFMEMORY;
    *enumerator = clone;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Next(ULONG count, ITfDisplayAttributeInfo **items,
                                 ULONG *fetched) override {
    ULONG produced = 0;
    if (count != 0 && !items)
      return E_POINTER;
    if (count != 0 && !consumed_) {
      const HRESULT result =
          CreateCompositionDisplayAttributeInfo(&items[0]);
      if (FAILED(result))
        return result;
      consumed_ = true;
      produced = 1;
    }
    if (fetched)
      *fetched = produced;
    return produced == count ? S_OK : S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE Reset() override {
    consumed_ = false;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
    if (count == 0)
      return S_OK;
    if (consumed_)
      return S_FALSE;
    consumed_ = true;
    return count == 1 ? S_OK : S_FALSE;
  }

private:
  ~DisplayAttributeInfoEnum() { RemoveModuleObject(); }

  std::atomic<ULONG> references_{1};
  bool consumed_ = false;
};

} // namespace

HRESULT CreateCompositionDisplayAttributeInfo(
    ITfDisplayAttributeInfo **info) noexcept {
  if (!info)
    return E_POINTER;
  *info = nullptr;
  auto *created = new (std::nothrow) DisplayAttributeInfo();
  if (!created)
    return E_OUTOFMEMORY;
  *info = created;
  return S_OK;
}

HRESULT CreateDisplayAttributeInfoEnum(
    IEnumTfDisplayAttributeInfo **enumerator) noexcept {
  if (!enumerator)
    return E_POINTER;
  *enumerator = nullptr;
  auto *created = new (std::nothrow) DisplayAttributeInfoEnum();
  if (!created)
    return E_OUTOFMEMORY;
  *enumerator = created;
  return S_OK;
}

} // namespace famo::tsf
