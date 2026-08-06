#include "search_candidate_list.h"

#include <atomic>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include <oleauto.h>

#include "abi_boundary.h"
#include "module_state.h"

namespace famo::tsf {
namespace {

using CandidateValues = std::vector<std::wstring>;

class CandidateString final : public ITfCandidateString {
public:
  CandidateString(std::shared_ptr<const CandidateValues> values, ULONG index)
      : values_(std::move(values)), index_(index) {
    AddModuleObject();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfCandidateString)
      *object = static_cast<ITfCandidateString *>(this);
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

  HRESULT STDMETHODCALLTYPE GetString(BSTR *value) override {
    if (!value)
      return E_POINTER;
    *value = nullptr;
    const std::wstring &candidate = (*values_)[index_];
    *value = SysAllocStringLen(candidate.data(),
                               static_cast<UINT>(candidate.size()));
    return *value ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT STDMETHODCALLTYPE GetIndex(ULONG *index) override {
    if (!index)
      return E_POINTER;
    *index = index_;
    return S_OK;
  }

private:
  ~CandidateString() { RemoveModuleObject(); }

  std::atomic<ULONG> references_{1};
  std::shared_ptr<const CandidateValues> values_;
  ULONG index_ = 0;
};

class CandidateEnumerator final : public IEnumTfCandidates {
public:
  CandidateEnumerator(std::shared_ptr<const CandidateValues> values,
                      size_t position = 0)
      : values_(std::move(values)), position_(position) {
    AddModuleObject();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IEnumTfCandidates)
      *object = static_cast<IEnumTfCandidates *>(this);
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

  HRESULT STDMETHODCALLTYPE Clone(IEnumTfCandidates **enumerator) override {
    if (!enumerator)
      return E_POINTER;
    *enumerator = nullptr;
    auto *copy = new (std::nothrow) CandidateEnumerator(values_, position_);
    if (!copy)
      return E_OUTOFMEMORY;
    *enumerator = copy;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Next(ULONG count, ITfCandidateString **candidates,
                                 ULONG *fetched) override {
    if (count != 0 && !candidates)
      return E_INVALIDARG;
    ULONG local_fetched = 0;
    ULONG *written = fetched ? fetched : &local_fetched;
    *written = 0;
    if (count == 0)
      return S_OK;

    const size_t original_position = position_;
    while (*written < count && position_ < values_->size()) {
      auto *candidate = new (std::nothrow)
          CandidateString(values_, static_cast<ULONG>(position_));
      if (!candidate) {
        for (ULONG index = 0; index < *written; ++index) {
          candidates[index]->Release();
          candidates[index] = nullptr;
        }
        position_ = original_position;
        *written = 0;
        return E_OUTOFMEMORY;
      }
      candidates[*written] = candidate;
      ++*written;
      ++position_;
    }
    return *written == count ? S_OK : S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE Reset() override {
    position_ = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
    const size_t remaining = values_->size() - position_;
    if (static_cast<size_t>(count) <= remaining) {
      position_ += count;
      return S_OK;
    }
    position_ = values_->size();
    return S_FALSE;
  }

private:
  ~CandidateEnumerator() { RemoveModuleObject(); }

  std::atomic<ULONG> references_{1};
  std::shared_ptr<const CandidateValues> values_;
  size_t position_ = 0;
};

class SearchCandidateList final : public ITfCandidateList {
public:
  explicit SearchCandidateList(std::shared_ptr<const CandidateValues> values)
      : values_(std::move(values)) {
    AddModuleObject();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfCandidateList)
      *object = static_cast<ITfCandidateList *>(this);
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
  EnumCandidates(IEnumTfCandidates **enumerator) override {
    if (!enumerator)
      return E_POINTER;
    *enumerator = nullptr;
    auto *created = new (std::nothrow) CandidateEnumerator(values_);
    if (!created)
      return E_OUTOFMEMORY;
    *enumerator = created;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetCandidate(ULONG index, ITfCandidateString **candidate) override {
    if (!candidate)
      return E_INVALIDARG;
    *candidate = nullptr;
    if (index >= values_->size())
      return E_FAIL;
    auto *created = new (std::nothrow) CandidateString(values_, index);
    if (!created)
      return E_OUTOFMEMORY;
    *candidate = created;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetCandidateNum(ULONG *count) override {
    if (!count)
      return E_POINTER;
    *count = static_cast<ULONG>(values_->size());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetResult(ULONG index,
                                      TfCandidateResult result) override {
    if (result != CAND_FINALIZED && result != CAND_SELECTED &&
        result != CAND_CANCELED) {
      return E_INVALIDARG;
    }
    if (result != CAND_CANCELED && index >= values_->size())
      return E_INVALIDARG;
    // Search candidate lists are immutable. This acknowledges the host's
    // advisory result without mutating the engine or an IME session.
    return S_OK;
  }

private:
  ~SearchCandidateList() { RemoveModuleObject(); }

  std::atomic<ULONG> references_{1};
  std::shared_ptr<const CandidateValues> values_;
};

} // namespace

HRESULT CreateSearchCandidateList(std::vector<std::wstring> candidates,
                                  ITfCandidateList **list) noexcept {
  return ComBoundary([&]() -> HRESULT {
    if (!list)
      return E_POINTER;
    *list = nullptr;
    if (candidates.size() > std::numeric_limits<ULONG>::max())
      return E_INVALIDARG;
    for (const std::wstring &candidate : candidates) {
      if (candidate.size() > std::numeric_limits<UINT>::max())
        return E_INVALIDARG;
    }
    auto values =
        std::make_shared<const CandidateValues>(std::move(candidates));
    auto *created = new (std::nothrow) SearchCandidateList(std::move(values));
    if (!created)
      return E_OUTOFMEMORY;
    *list = created;
    return S_OK;
  });
}

} // namespace famo::tsf
