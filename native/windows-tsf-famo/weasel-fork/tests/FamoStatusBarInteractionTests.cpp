#include "../overlay/include/FamoStatusBarInteraction.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

FamoStatusBarInteractionModel MakeModel() {
  FamoStatusBarInteractionModel model;
  model.SetButtonRects({
      {0, 0, 34, 34},
      {36, 0, 70, 34},
  });
  return model;
}

void PressDragOutsideCancelsClickEvenIfReleasedInside() {
  auto model = MakeModel();

  model.LeftDown({10, 10});
  Expect(model.PressIndex() == 0, "press starts on the first button");
  Expect(model.MouseMove({40, 10}), "moving to another button changes interaction state");
  Expect(model.HoverIndex() == 1, "hover follows the pointer");
  Expect(model.PressOutside(), "press is marked outside once pointer leaves original button");

  FamoStatusBarClick click = model.LeftUp({10, 10}, false);
  Expect(!click.has_value, "release inside original button after leaving must not click");
  Expect(model.PressIndex() == -1, "press state resets after release");
}

void PlainClickFiresPressedButton() {
  auto model = MakeModel();

  model.LeftDown({10, 10});
  FamoStatusBarClick click = model.LeftUp({10, 10}, false);
  Expect(click.has_value, "plain click should fire");
  Expect(click.index == 0, "plain click fires the pressed button");
}

void MouseLeaveClearsHoverButKeepsPressCancellation() {
  auto model = MakeModel();

  model.MouseMove({10, 10});
  Expect(model.HoverIndex() == 0, "hover starts on first button");
  Expect(model.MouseLeave(), "mouse leave changes hover state");
  Expect(model.HoverIndex() == -1, "mouse leave clears hover");

  model.LeftDown({10, 10});
  model.MouseLeave();
  Expect(model.PressOutside(), "leaving while pressed cancels click");
  FamoStatusBarClick click = model.LeftUp({10, 10}, false);
  Expect(!click.has_value, "leave while pressed prevents click");
}

void CaptureChangedClearsTransientState() {
  auto model = MakeModel();

  model.LeftDown({10, 10});
  model.MouseMove({40, 10});
  Expect(model.CaptureChanged(), "capture change clears active state");
  Expect(model.HoverIndex() == -1, "capture change clears hover");
  Expect(model.PressIndex() == -1, "capture change clears press");
  Expect(!model.PressOutside(), "capture change clears cancellation flag");
}

}  // namespace

int main() {
  PressDragOutsideCancelsClickEvenIfReleasedInside();
  PlainClickFiresPressedButton();
  MouseLeaveClearsHoverButKeepsPressCancellation();
  CaptureChangedClearsTransientState();
  std::cout << "FamoStatusBarInteractionTests passed\n";
  return 0;
}
