#include "candidate_automation.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <objbase.h>

#include <UIAutomation.h>

namespace famo::runtime {
namespace {

struct AutomationItem {
  std::wstring name;
  RECT bounds{};
  bool offscreen = false;
};

struct AutomationState {
  bool visible = false;
  RECT bounds{};
  std::vector<AutomationItem> items;
  uint32_t selected = 0;
  uint32_t tree_generation = 1;
};

bool Utf8ToWide(std::string_view input, std::wstring *output) {
  if (!output || input.empty() ||
      input.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                          static_cast<int>(input.size()), nullptr, 0);
  if (size <= 0)
    return false;
  std::wstring converted(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                          static_cast<int>(input.size()), converted.data(),
                          size) != size) {
    return false;
  }
  *output = std::move(converted);
  return true;
}

void SetBool(VARIANT *value, bool enabled) {
  value->vt = VT_BOOL;
  value->boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
}

void SetInt(VARIANT *value, LONG number) {
  value->vt = VT_I4;
  value->lVal = number;
}

bool SameRect(const RECT &left, const RECT &right) {
  return EqualRect(&left, &right) != FALSE;
}

RECT ClipToBounds(const RECT &value, const RECT &bounds, bool *offscreen) {
  RECT clipped{};
  const bool visible = IntersectRect(&clipped, &value, &bounds) != FALSE;
  if (offscreen)
    *offscreen = !visible;
  return visible ? clipped
                 : RECT{bounds.left, bounds.top, bounds.left, bounds.top};
}

bool SetRect(VARIANT *value, const RECT &bounds) {
  if (!value)
    return false;
  VariantInit(value);
  SAFEARRAY *array = SafeArrayCreateVector(VT_R8, 0, 4);
  if (!array)
    return false;
  double *items = nullptr;
  if (FAILED(SafeArrayAccessData(array, reinterpret_cast<void **>(&items)))) {
    SafeArrayDestroy(array);
    return false;
  }
  items[0] = static_cast<double>(bounds.left);
  items[1] = static_cast<double>(bounds.top);
  items[2] = static_cast<double>(bounds.right - bounds.left);
  items[3] = static_cast<double>(bounds.bottom - bounds.top);
  SafeArrayUnaccessData(array);
  value->vt = VT_ARRAY | VT_R8;
  value->parray = array;
  return true;
}

void RaiseBoolProperty(IRawElementProviderSimple *provider, PROPERTYID property,
                       bool previous, bool next) {
  if (!provider || previous == next)
    return;
  VARIANT old_value;
  VARIANT new_value;
  VariantInit(&old_value);
  VariantInit(&new_value);
  SetBool(&old_value, previous);
  SetBool(&new_value, next);
  (void)UiaRaiseAutomationPropertyChangedEvent(provider, property, old_value,
                                               new_value);
}

void RaiseBoundsProperty(IRawElementProviderSimple *provider,
                         const RECT &previous, const RECT &next) {
  if (!provider || SameRect(previous, next))
    return;
  VARIANT old_value;
  VARIANT new_value;
  if (!SetRect(&old_value, previous))
    return;
  if (!SetRect(&new_value, next)) {
    VariantClear(&old_value);
    return;
  }
  (void)UiaRaiseAutomationPropertyChangedEvent(
      provider, UIA_BoundingRectanglePropertyId, old_value, new_value);
  VariantClear(&new_value);
  VariantClear(&old_value);
}

HRESULT SetString(VARIANT *value, const wchar_t *text) {
  value->vt = VT_BSTR;
  value->bstrVal = SysAllocString(text);
  if (!value->bstrVal) {
    value->vt = VT_EMPTY;
    return E_OUTOFMEMORY;
  }
  return S_OK;
}

UiaRect ToUiaRect(const RECT &bounds) {
  return {static_cast<double>(bounds.left), static_cast<double>(bounds.top),
          static_cast<double>(bounds.right - bounds.left),
          static_cast<double>(bounds.bottom - bounds.top)};
}

bool SameItems(const AutomationState &state,
               const std::vector<AutomationItem> &items) {
  if (state.items.size() != items.size())
    return false;
  for (size_t index = 0; index < items.size(); ++index) {
    if (state.items[index].name != items[index].name)
      return false;
  }
  return true;
}

class AutomationRoot;

class AutomationChild final : public IRawElementProviderSimple,
                              public IRawElementProviderFragment,
                              public ISelectionItemProvider {
public:
  AutomationChild(AutomationRoot *root, uint32_t tree_generation,
                  uint32_t index);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE
  get_ProviderOptions(ProviderOptions *options) override;
  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern,
                                               IUnknown **provider) override;
  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,
                                             VARIANT *value) override;
  HRESULT STDMETHODCALLTYPE
  get_HostRawElementProvider(IRawElementProviderSimple **provider) override;

  HRESULT STDMETHODCALLTYPE
  Navigate(NavigateDirection direction,
           IRawElementProviderFragment **provider) override;
  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY **runtime_id) override;
  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect *bounds) override;
  HRESULT STDMETHODCALLTYPE
  GetEmbeddedFragmentRoots(SAFEARRAY **roots) override;
  HRESULT STDMETHODCALLTYPE SetFocus() override;
  HRESULT STDMETHODCALLTYPE
  get_FragmentRoot(IRawElementProviderFragmentRoot **root) override;

  HRESULT STDMETHODCALLTYPE Select() override;
  HRESULT STDMETHODCALLTYPE AddToSelection() override;
  HRESULT STDMETHODCALLTYPE RemoveFromSelection() override;
  HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL *selected) override;
  HRESULT STDMETHODCALLTYPE
  get_SelectionContainer(IRawElementProviderSimple **container) override;

private:
  ~AutomationChild();

  std::atomic<ULONG> references_{1};
  AutomationRoot *root_ = nullptr;
  uint32_t tree_generation_ = 0;
  uint32_t index_ = 0;
};

class AutomationRoot final : public IRawElementProviderSimple,
                             public IRawElementProviderFragment,
                             public IRawElementProviderFragmentRoot,
                             public ISelectionProvider {
public:
  explicit AutomationRoot(HWND window) : window_(window) {
    state_.store(std::make_shared<const AutomationState>());
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple)
      *object = static_cast<IRawElementProviderSimple *>(this);
    else if (iid == IID_IRawElementProviderFragment)
      *object = static_cast<IRawElementProviderFragment *>(this);
    else if (iid == IID_IRawElementProviderFragmentRoot)
      *object = static_cast<IRawElementProviderFragmentRoot *>(this);
    else if (iid == IID_ISelectionProvider)
      *object = static_cast<ISelectionProvider *>(this);
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
  get_ProviderOptions(ProviderOptions *options) override {
    if (!options)
      return E_POINTER;
    *options = static_cast<ProviderOptions>(
        ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading |
        ProviderOptions_RefuseNonClientSupport |
        ProviderOptions_ProviderOwnsSetFocus);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern,
                                               IUnknown **provider) override {
    if (!provider)
      return E_POINTER;
    *provider = nullptr;
    if (pattern == UIA_SelectionPatternId) {
      *provider = static_cast<ISelectionProvider *>(this);
      AddRef();
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,
                                             VARIANT *value) override {
    if (!value)
      return E_POINTER;
    VariantInit(value);
    const std::shared_ptr<const AutomationState> state = Current();
    if (!state)
      return UIA_E_ELEMENTNOTAVAILABLE;
    switch (property) {
    case UIA_ControlTypePropertyId:
      SetInt(value, UIA_ListControlTypeId);
      break;
    case UIA_AutomationIdPropertyId:
      return SetString(value, L"IME_Candidate_Window");
    case UIA_NamePropertyId:
      return SetString(value, L"Famo candidates");
    case UIA_ClassNamePropertyId:
      return SetString(value, L"FamoRuntimeCandidateWindow");
    case UIA_FrameworkIdPropertyId:
      return SetString(value, L"Win32");
    case UIA_IsControlElementPropertyId:
    case UIA_IsContentElementPropertyId:
    case UIA_IsEnabledPropertyId:
    case UIA_IsSelectionPatternAvailablePropertyId:
      SetBool(value, true);
      break;
    case UIA_IsKeyboardFocusablePropertyId:
    case UIA_HasKeyboardFocusPropertyId:
      SetBool(value, false);
      break;
    case UIA_IsOffscreenPropertyId:
      SetBool(value, !state->visible);
      break;
    default:
      break;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  get_HostRawElementProvider(IRawElementProviderSimple **provider) override {
    if (!provider)
      return E_POINTER;
    *provider = nullptr;
    const HWND window = window_.load();
    return window ? UiaHostProviderFromHwnd(window, provider)
                  : UIA_E_ELEMENTNOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE
  Navigate(NavigateDirection direction,
           IRawElementProviderFragment **provider) override;

  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY **runtime_id) override {
    if (!runtime_id)
      return E_POINTER;
    // A top-level fragment hosted by an HWND inherits the HWND runtime ID.
    *runtime_id = nullptr;
    return Current() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect *bounds) override {
    if (!bounds)
      return E_POINTER;
    const std::shared_ptr<const AutomationState> state = Current();
    if (!state)
      return UIA_E_ELEMENTNOTAVAILABLE;
    *bounds = ToUiaRect(state->bounds);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetEmbeddedFragmentRoots(SAFEARRAY **roots) override {
    if (!roots)
      return E_POINTER;
    *roots = nullptr;
    return Current() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE SetFocus() override {
    return Current() ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE
  get_FragmentRoot(IRawElementProviderFragmentRoot **root) override {
    if (!root)
      return E_POINTER;
    *root = nullptr;
    if (!Current())
      return UIA_E_ELEMENTNOTAVAILABLE;
    *root = static_cast<IRawElementProviderFragmentRoot *>(this);
    AddRef();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(
      double x, double y, IRawElementProviderFragment **provider) override;

  HRESULT STDMETHODCALLTYPE
  GetFocus(IRawElementProviderFragment **provider) override {
    if (!provider)
      return E_POINTER;
    *provider = nullptr;
    return Current() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY **selection) override;

  HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL *multiple) override {
    if (!multiple)
      return E_POINTER;
    *multiple = FALSE;
    return Current() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL *required) override {
    if (!required)
      return E_POINTER;
    *required = TRUE;
    return Current() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }

  std::shared_ptr<const AutomationState> Current() const noexcept {
    if (!window_.load(std::memory_order_acquire))
      return nullptr;
    return state_.load(std::memory_order_acquire);
  }

  std::shared_ptr<const AutomationState>
  CurrentItem(uint32_t generation, uint32_t index) const noexcept {
    std::shared_ptr<const AutomationState> state = Current();
    if (!state || state->tree_generation != generation ||
        index >= state->items.size()) {
      return nullptr;
    }
    return state;
  }

  AutomationChild *CreateChild(const AutomationState &state,
                               uint32_t index) noexcept {
    if (index >= state.items.size())
      return nullptr;
    return new (std::nothrow)
        AutomationChild(this, state.tree_generation, index);
  }

  bool RequestSelection(uint32_t generation, uint32_t index) noexcept {
    const std::shared_ptr<const AutomationState> state =
        CurrentItem(generation, index);
    const HWND window = window_.load(std::memory_order_acquire);
    return state && state->visible && window &&
           PostMessageW(window, kCandidateAutomationSelectMessage,
                        static_cast<WPARAM>(generation),
                        static_cast<LPARAM>(index)) != FALSE;
  }

  bool IsCurrentItem(uint32_t generation, uint32_t index) const noexcept {
    const std::shared_ptr<const AutomationState> state =
        CurrentItem(generation, index);
    return state && state->visible;
  }

  void Present(const Composition &composition, const FamoLayoutResult &layout,
               int shadow_margin) {
    const uint32_t count =
        (std::min)(static_cast<uint32_t>(composition.candidates.size()),
                   (std::min)(layout.candidate_count,
                              static_cast<uint32_t>(FAMO_MAX_LAID_CANDIDATES)));
    if (count == 0) {
      Hide();
      return;
    }
    const RECT root_bounds = {
        layout.origin_x - shadow_margin, layout.origin_y - shadow_margin,
        layout.origin_x + layout.content_size.cx + shadow_margin,
        layout.origin_y + layout.content_size.cy + shadow_margin};
    std::vector<AutomationItem> items;
    items.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
      std::wstring name;
      if (!Utf8ToWide(composition.candidates[index].text, &name)) {
        Hide();
        return;
      }
      const FamoRect &source = layout.candidates[index].bounds;
      const RECT item_bounds = {
          layout.origin_x + source.left, layout.origin_y + source.top,
          layout.origin_x + source.right, layout.origin_y + source.bottom};
      bool offscreen = false;
      items.push_back({std::move(name),
                       ClipToBounds(item_bounds, root_bounds, &offscreen),
                       offscreen});
    }

    const std::shared_ptr<const AutomationState> previous =
        state_.load(std::memory_order_acquire);
    auto next = std::make_shared<AutomationState>();
    next->visible = true;
    next->bounds = root_bounds;
    next->items = std::move(items);
    next->selected = composition.highlighted_index < count
                         ? composition.highlighted_index
                         : 0;
    next->tree_generation = previous ? previous->tree_generation : 1;
    const bool same_items = previous && SameItems(*previous, next->items);
    if (!same_items) {
      ++next->tree_generation;
      if (next->tree_generation == 0)
        ++next->tree_generation;
    }
    const bool was_visible = previous && previous->visible;
    const bool selection_changed =
        was_visible && (!same_items || previous->selected != next->selected);
    const uint32_t selected = next->selected;
    state_.store(next, std::memory_order_release);

    auto *root_provider = static_cast<IRawElementProviderSimple *>(this);

    if (!was_visible) {
      RaiseBoolProperty(root_provider, UIA_IsOffscreenPropertyId, true, false);
      if (previous)
        RaiseBoundsProperty(root_provider, previous->bounds, next->bounds);
      (void)UiaRaiseStructureChangedEvent(
          root_provider, StructureChangeType_ChildrenInvalidated, nullptr, 0);
      (void)UiaRaiseAutomationEvent(root_provider,
                                    UIA_LayoutInvalidatedEventId);
      (void)UiaRaiseAutomationEvent(root_provider,
                                    UIA_Selection_InvalidatedEventId);
      (void)UiaRaiseAutomationEvent(root_provider, UIA_MenuOpenedEventId);
      return;
    }

    bool layout_changed = !SameRect(previous->bounds, next->bounds);
    RaiseBoundsProperty(root_provider, previous->bounds, next->bounds);
    if (!same_items) {
      (void)UiaRaiseStructureChangedEvent(
          root_provider, StructureChangeType_ChildrenInvalidated, nullptr, 0);
      layout_changed = true;
    } else {
      for (uint32_t index = 0; index < count; ++index) {
        const bool bounds_changed =
            !SameRect(previous->items[index].bounds, next->items[index].bounds);
        const bool offscreen_changed =
            previous->items[index].offscreen != next->items[index].offscreen;
        if (!bounds_changed && !offscreen_changed)
          continue;
        AutomationChild *child = CreateChild(*next, index);
        if (!child)
          continue;
        auto *child_provider = static_cast<IRawElementProviderSimple *>(child);
        RaiseBoundsProperty(child_provider, previous->items[index].bounds,
                            next->items[index].bounds);
        RaiseBoolProperty(child_provider, UIA_IsOffscreenPropertyId,
                          previous->items[index].offscreen,
                          next->items[index].offscreen);
        child->Release();
        layout_changed = true;
      }
    }
    if (layout_changed)
      (void)UiaRaiseAutomationEvent(root_provider,
                                    UIA_LayoutInvalidatedEventId);

    if (selection_changed) {
      if (same_items && previous->selected != selected) {
        AutomationChild *old_child = CreateChild(*next, previous->selected);
        if (old_child) {
          RaiseBoolProperty(static_cast<IRawElementProviderSimple *>(old_child),
                            UIA_SelectionItemIsSelectedPropertyId, true, false);
          old_child->Release();
        }
        AutomationChild *new_child = CreateChild(*next, selected);
        if (new_child) {
          RaiseBoolProperty(static_cast<IRawElementProviderSimple *>(new_child),
                            UIA_SelectionItemIsSelectedPropertyId, false, true);
          new_child->Release();
        }
      }
      (void)UiaRaiseAutomationEvent(root_provider,
                                    UIA_Selection_InvalidatedEventId);
      AutomationChild *child = CreateChild(*next, selected);
      if (child) {
        (void)UiaRaiseAutomationEvent(
            static_cast<IRawElementProviderSimple *>(child),
            UIA_SelectionItem_ElementSelectedEventId);
        child->Release();
      }
    }
  }

  void Move(const FamoLayoutResult &layout, int shadow_margin) {
    const std::shared_ptr<const AutomationState> previous =
        state_.load(std::memory_order_acquire);
    if (!previous || previous->items.size() != layout.candidate_count)
      return;
    auto next = std::make_shared<AutomationState>(*previous);
    next->bounds = {layout.origin_x - shadow_margin,
                    layout.origin_y - shadow_margin,
                    layout.origin_x + layout.content_size.cx + shadow_margin,
                    layout.origin_y + layout.content_size.cy + shadow_margin};
    for (uint32_t index = 0; index < layout.candidate_count; ++index) {
      const FamoRect &source = layout.candidates[index].bounds;
      const RECT item_bounds = {
          layout.origin_x + source.left, layout.origin_y + source.top,
          layout.origin_x + source.right, layout.origin_y + source.bottom};
      next->items[index].bounds = ClipToBounds(item_bounds, next->bounds,
                                               &next->items[index].offscreen);
    }
    state_.store(next, std::memory_order_release);
    auto *root_provider = static_cast<IRawElementProviderSimple *>(this);
    bool layout_changed = !SameRect(previous->bounds, next->bounds);
    RaiseBoundsProperty(root_provider, previous->bounds, next->bounds);
    for (uint32_t index = 0; index < layout.candidate_count; ++index) {
      const bool bounds_changed =
          !SameRect(previous->items[index].bounds, next->items[index].bounds);
      const bool offscreen_changed =
          previous->items[index].offscreen != next->items[index].offscreen;
      if (!bounds_changed && !offscreen_changed)
        continue;
      AutomationChild *child = CreateChild(*next, index);
      if (!child)
        continue;
      auto *child_provider = static_cast<IRawElementProviderSimple *>(child);
      RaiseBoundsProperty(child_provider, previous->items[index].bounds,
                          next->items[index].bounds);
      RaiseBoolProperty(child_provider, UIA_IsOffscreenPropertyId,
                        previous->items[index].offscreen,
                        next->items[index].offscreen);
      child->Release();
      layout_changed = true;
    }
    if (layout_changed)
      (void)UiaRaiseAutomationEvent(root_provider,
                                    UIA_LayoutInvalidatedEventId);
  }

  void Hide() noexcept {
    try {
      const std::shared_ptr<const AutomationState> previous =
          state_.load(std::memory_order_acquire);
      if (!previous || !previous->visible)
        return;
      auto hidden = std::make_shared<AutomationState>(*previous);
      hidden->visible = false;
      hidden->items.clear();
      hidden->selected = 0;
      ++hidden->tree_generation;
      if (hidden->tree_generation == 0)
        ++hidden->tree_generation;
      state_.store(std::move(hidden), std::memory_order_release);
      auto *root_provider = static_cast<IRawElementProviderSimple *>(this);
      RaiseBoolProperty(root_provider, UIA_IsOffscreenPropertyId, false, true);
      (void)UiaRaiseStructureChangedEvent(
          root_provider, StructureChangeType_ChildrenInvalidated, nullptr, 0);
      (void)UiaRaiseAutomationEvent(root_provider,
                                    UIA_LayoutInvalidatedEventId);
      (void)UiaRaiseAutomationEvent(root_provider,
                                    UIA_Selection_InvalidatedEventId);
      (void)UiaRaiseAutomationEvent(root_provider, UIA_MenuClosedEventId);
    } catch (...) {
    }
  }

  LRESULT HandleGetObject(WPARAM wparam, LPARAM lparam) noexcept {
    const HWND window = window_.load(std::memory_order_acquire);
    if (!window || static_cast<LONG>(lparam) != UiaRootObjectId)
      return 0;
    return UiaReturnRawElementProvider(
        window, wparam, lparam, static_cast<IRawElementProviderSimple *>(this));
  }

  void WindowDestroyed() noexcept {
    const HWND window = window_.load(std::memory_order_acquire);
    if (!window)
      return;
    Hide();
    window_.store(nullptr, std::memory_order_release);
    (void)UiaReturnRawElementProvider(window, 0, 0, nullptr);
    (void)UiaDisconnectProvider(static_cast<IRawElementProviderSimple *>(this));
  }

private:
  ~AutomationRoot() = default;

  std::atomic<ULONG> references_{1};
  std::atomic<HWND> window_{nullptr};
  std::atomic<std::shared_ptr<const AutomationState>> state_;
};

AutomationChild::AutomationChild(AutomationRoot *root, uint32_t tree_generation,
                                 uint32_t index)
    : root_(root), tree_generation_(tree_generation), index_(index) {
  root_->AddRef();
}

AutomationChild::~AutomationChild() { root_->Release(); }

HRESULT AutomationChild::QueryInterface(REFIID iid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple)
    *object = static_cast<IRawElementProviderSimple *>(this);
  else if (iid == IID_IRawElementProviderFragment)
    *object = static_cast<IRawElementProviderFragment *>(this);
  else if (iid == IID_ISelectionItemProvider)
    *object = static_cast<ISelectionItemProvider *>(this);
  if (!*object)
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

ULONG AutomationChild::AddRef() { return ++references_; }

ULONG AutomationChild::Release() {
  const ULONG remaining = --references_;
  if (remaining == 0)
    delete this;
  return remaining;
}

HRESULT AutomationChild::get_ProviderOptions(ProviderOptions *options) {
  if (!options)
    return E_POINTER;
  *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider |
                                          ProviderOptions_UseComThreading |
                                          ProviderOptions_ProviderOwnsSetFocus);
  return S_OK;
}

HRESULT AutomationChild::GetPatternProvider(PATTERNID pattern,
                                            IUnknown **provider) {
  if (!provider)
    return E_POINTER;
  *provider = nullptr;
  if (pattern == UIA_SelectionItemPatternId) {
    *provider = static_cast<ISelectionItemProvider *>(this);
    AddRef();
  }
  return S_OK;
}

HRESULT AutomationChild::GetPropertyValue(PROPERTYID property, VARIANT *value) {
  if (!value)
    return E_POINTER;
  VariantInit(value);
  const std::shared_ptr<const AutomationState> state =
      root_->CurrentItem(tree_generation_, index_);
  if (!state)
    return UIA_E_ELEMENTNOTAVAILABLE;
  switch (property) {
  case UIA_ControlTypePropertyId:
    SetInt(value, UIA_ListItemControlTypeId);
    break;
  case UIA_NamePropertyId:
    value->vt = VT_BSTR;
    value->bstrVal =
        SysAllocStringLen(state->items[index_].name.data(),
                          static_cast<UINT>(state->items[index_].name.size()));
    if (!value->bstrVal) {
      value->vt = VT_EMPTY;
      return E_OUTOFMEMORY;
    }
    break;
  case UIA_IsControlElementPropertyId:
  case UIA_IsContentElementPropertyId:
  case UIA_IsEnabledPropertyId:
  case UIA_IsSelectionItemPatternAvailablePropertyId:
    SetBool(value, true);
    break;
  case UIA_IsKeyboardFocusablePropertyId:
  case UIA_HasKeyboardFocusPropertyId:
    SetBool(value, false);
    break;
  case UIA_IsOffscreenPropertyId:
    SetBool(value, !state->visible || state->items[index_].offscreen);
    break;
  case UIA_SelectionItemIsSelectedPropertyId:
    SetBool(value, state->visible && state->selected == index_);
    break;
  default:
    break;
  }
  return S_OK;
}

HRESULT AutomationChild::get_HostRawElementProvider(
    IRawElementProviderSimple **provider) {
  if (!provider)
    return E_POINTER;
  *provider = nullptr;
  return root_->CurrentItem(tree_generation_, index_)
             ? S_OK
             : UIA_E_ELEMENTNOTAVAILABLE;
}

HRESULT AutomationChild::Navigate(NavigateDirection direction,
                                  IRawElementProviderFragment **provider) {
  if (!provider)
    return E_POINTER;
  *provider = nullptr;
  const std::shared_ptr<const AutomationState> state =
      root_->CurrentItem(tree_generation_, index_);
  if (!state)
    return UIA_E_ELEMENTNOTAVAILABLE;
  if (direction == NavigateDirection_Parent) {
    *provider = static_cast<IRawElementProviderFragment *>(root_);
    root_->AddRef();
    return S_OK;
  }
  uint32_t target = index_;
  if (direction == NavigateDirection_NextSibling) {
    if (index_ + 1 >= state->items.size())
      return S_OK;
    target = index_ + 1;
  } else if (direction == NavigateDirection_PreviousSibling) {
    if (index_ == 0)
      return S_OK;
    target = index_ - 1;
  } else {
    return S_OK;
  }
  AutomationChild *child = root_->CreateChild(*state, target);
  if (!child)
    return E_OUTOFMEMORY;
  *provider = static_cast<IRawElementProviderFragment *>(child);
  return S_OK;
}

HRESULT AutomationChild::GetRuntimeId(SAFEARRAY **runtime_id) {
  if (!runtime_id)
    return E_POINTER;
  *runtime_id = nullptr;
  if (!root_->CurrentItem(tree_generation_, index_))
    return UIA_E_ELEMENTNOTAVAILABLE;
  SAFEARRAY *created = SafeArrayCreateVector(VT_I4, 0, 3);
  if (!created)
    return E_OUTOFMEMORY;
  LONG *values = nullptr;
  HRESULT result =
      SafeArrayAccessData(created, reinterpret_cast<void **>(&values));
  if (FAILED(result)) {
    SafeArrayDestroy(created);
    return result;
  }
  values[0] = UiaAppendRuntimeId;
  values[1] = static_cast<LONG>(tree_generation_);
  values[2] = static_cast<LONG>(index_ + 1);
  SafeArrayUnaccessData(created);
  *runtime_id = created;
  return S_OK;
}

HRESULT AutomationChild::get_BoundingRectangle(UiaRect *bounds) {
  if (!bounds)
    return E_POINTER;
  const std::shared_ptr<const AutomationState> state =
      root_->CurrentItem(tree_generation_, index_);
  if (!state)
    return UIA_E_ELEMENTNOTAVAILABLE;
  *bounds = ToUiaRect(state->items[index_].bounds);
  return S_OK;
}

HRESULT AutomationChild::GetEmbeddedFragmentRoots(SAFEARRAY **roots) {
  if (!roots)
    return E_POINTER;
  *roots = nullptr;
  return root_->CurrentItem(tree_generation_, index_)
             ? S_OK
             : UIA_E_ELEMENTNOTAVAILABLE;
}

HRESULT AutomationChild::SetFocus() {
  return root_->CurrentItem(tree_generation_, index_)
             ? UIA_E_NOTSUPPORTED
             : UIA_E_ELEMENTNOTAVAILABLE;
}

HRESULT
AutomationChild::get_FragmentRoot(IRawElementProviderFragmentRoot **root) {
  if (!root)
    return E_POINTER;
  *root = nullptr;
  if (!root_->CurrentItem(tree_generation_, index_))
    return UIA_E_ELEMENTNOTAVAILABLE;
  *root = static_cast<IRawElementProviderFragmentRoot *>(root_);
  root_->AddRef();
  return S_OK;
}

HRESULT AutomationChild::Select() {
  return root_->RequestSelection(tree_generation_, index_)
             ? S_OK
             : UIA_E_ELEMENTNOTAVAILABLE;
}

HRESULT AutomationChild::AddToSelection() { return Select(); }

HRESULT AutomationChild::RemoveFromSelection() {
  return root_->CurrentItem(tree_generation_, index_)
             ? UIA_E_INVALIDOPERATION
             : UIA_E_ELEMENTNOTAVAILABLE;
}

HRESULT AutomationChild::get_IsSelected(BOOL *selected) {
  if (!selected)
    return E_POINTER;
  const std::shared_ptr<const AutomationState> state =
      root_->CurrentItem(tree_generation_, index_);
  if (!state)
    return UIA_E_ELEMENTNOTAVAILABLE;
  *selected = state->visible && state->selected == index_ ? TRUE : FALSE;
  return S_OK;
}

HRESULT
AutomationChild::get_SelectionContainer(IRawElementProviderSimple **container) {
  if (!container)
    return E_POINTER;
  *container = nullptr;
  if (!root_->CurrentItem(tree_generation_, index_))
    return UIA_E_ELEMENTNOTAVAILABLE;
  *container = static_cast<IRawElementProviderSimple *>(root_);
  root_->AddRef();
  return S_OK;
}

HRESULT AutomationRoot::Navigate(NavigateDirection direction,
                                 IRawElementProviderFragment **provider) {
  if (!provider)
    return E_POINTER;
  *provider = nullptr;
  const std::shared_ptr<const AutomationState> state = Current();
  if (!state)
    return UIA_E_ELEMENTNOTAVAILABLE;
  if (state->items.empty())
    return S_OK;
  uint32_t index = 0;
  if (direction == NavigateDirection_LastChild)
    index = static_cast<uint32_t>(state->items.size() - 1);
  else if (direction != NavigateDirection_FirstChild)
    return S_OK;
  AutomationChild *child = CreateChild(*state, index);
  if (!child)
    return E_OUTOFMEMORY;
  *provider = static_cast<IRawElementProviderFragment *>(child);
  return S_OK;
}

HRESULT AutomationRoot::ElementProviderFromPoint(
    double x, double y, IRawElementProviderFragment **provider) {
  if (!provider)
    return E_POINTER;
  *provider = nullptr;
  const std::shared_ptr<const AutomationState> state = Current();
  if (!state)
    return UIA_E_ELEMENTNOTAVAILABLE;
  if (state->visible) {
    for (uint32_t index = 0; index < state->items.size(); ++index) {
      const RECT &bounds = state->items[index].bounds;
      if (x >= bounds.left && x < bounds.right && y >= bounds.top &&
          y < bounds.bottom) {
        AutomationChild *child = CreateChild(*state, index);
        if (!child)
          return E_OUTOFMEMORY;
        *provider = static_cast<IRawElementProviderFragment *>(child);
        return S_OK;
      }
    }
  }
  *provider = static_cast<IRawElementProviderFragment *>(this);
  AddRef();
  return S_OK;
}

HRESULT AutomationRoot::GetSelection(SAFEARRAY **selection) {
  if (!selection)
    return E_POINTER;
  *selection = nullptr;
  const std::shared_ptr<const AutomationState> state = Current();
  if (!state)
    return UIA_E_ELEMENTNOTAVAILABLE;
  const ULONG count = state->visible && !state->items.empty() ? 1 : 0;
  SAFEARRAY *created = SafeArrayCreateVector(VT_UNKNOWN, 0, count);
  if (!created)
    return E_OUTOFMEMORY;
  if (count != 0) {
    AutomationChild *child = CreateChild(*state, state->selected);
    if (!child) {
      SafeArrayDestroy(created);
      return E_OUTOFMEMORY;
    }
    IUnknown *unknown = static_cast<IRawElementProviderSimple *>(child);
    LONG index = 0;
    const HRESULT result = SafeArrayPutElement(created, &index, unknown);
    child->Release();
    if (FAILED(result)) {
      SafeArrayDestroy(created);
      return result;
    }
  }
  *selection = created;
  return S_OK;
}

} // namespace

struct CandidateAutomation::Impl {
  explicit Impl(AutomationRoot *value) : root(value) {}
  ~Impl() {
    if (root)
      root->Release();
  }
  AutomationRoot *root = nullptr;
};

CandidateAutomation::CandidateAutomation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

std::unique_ptr<CandidateAutomation>
CandidateAutomation::Create(HWND window) noexcept {
  try {
    if (!window)
      return nullptr;
    auto *root = new (std::nothrow) AutomationRoot(window);
    if (!root)
      return nullptr;
    std::unique_ptr<Impl> impl;
    try {
      impl = std::make_unique<Impl>(root);
    } catch (...) {
      root->Release();
      return nullptr;
    }
    return std::unique_ptr<CandidateAutomation>(
        new (std::nothrow) CandidateAutomation(std::move(impl)));
  } catch (...) {
    return nullptr;
  }
}

CandidateAutomation::~CandidateAutomation() {
  if (impl_ && impl_->root)
    impl_->root->WindowDestroyed();
}

LRESULT CandidateAutomation::HandleGetObject(WPARAM wparam,
                                             LPARAM lparam) noexcept {
  return impl_ && impl_->root ? impl_->root->HandleGetObject(wparam, lparam)
                              : 0;
}

void CandidateAutomation::WindowDestroyed() noexcept {
  if (impl_ && impl_->root)
    impl_->root->WindowDestroyed();
}

void CandidateAutomation::Present(const Composition &composition,
                                  const FamoLayoutResult &layout,
                                  int shadow_margin) noexcept {
  try {
    if (impl_ && impl_->root)
      impl_->root->Present(composition, layout, shadow_margin);
  } catch (...) {
    Hide();
  }
}

void CandidateAutomation::Move(const FamoLayoutResult &layout,
                               int shadow_margin) noexcept {
  try {
    if (impl_ && impl_->root)
      impl_->root->Move(layout, shadow_margin);
  } catch (...) {
  }
}

void CandidateAutomation::Hide() noexcept {
  if (impl_ && impl_->root)
    impl_->root->Hide();
}

bool CandidateAutomation::IsCurrentItem(uint32_t generation,
                                        uint32_t index) const noexcept {
  return impl_ && impl_->root && impl_->root->IsCurrentItem(generation, index);
}

} // namespace famo::runtime
