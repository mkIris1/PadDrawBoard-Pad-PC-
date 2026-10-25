#include "pdb/input/profiles.h"

#include <cassert>

namespace pdb::input::tests {
void RunProfileResolverTests() {
  ProfileResolver resolver;
  resolver.SetProfiles({ProfileResolver::BlenderProfile()});

  const ApplicationProfile& blender = resolver.ResolveForExecutable(L"C:\\Program Files\\Blender\\BLENDER.EXE");
  assert(blender.buttons[0].kind == ActionKind::kMouseButton);
  assert(blender.buttons[0].mouseButton == MouseButton::kMiddle);
  assert(blender.buttons[1].virtualKeys.size() == 1);
  assert(blender.buttons[2].virtualKeys.size() == 1);

  const ApplicationProfile& fallback = resolver.ResolveForExecutable(L"paint.exe");
  assert(fallback.buttons[0].kind == ActionKind::kDisabled);
}
}  // namespace pdb::input::tests

#ifdef PDB_PROFILE_TEST_MAIN
int main() { pdb::input::tests::RunProfileResolverTests(); }
#endif
