#pragma once
#include <string>

// Test.InputScript: scripted mouse and keyboard input for unattended runs, ported from the PhysX project's record
// harness, so that tests can turn the camera and move the player and see the renderer in motion. One command a
// line, # starts a comment; times are gameplay seconds (as Test.ShotsAt's):
//   <t> look <dx a second> <dy a second> <duration s>   relative mouse movement in counts (the game reads whole
//                                                      counts a frame: the fraction is carried to the next frame)
//   <t> key <keen button id 0-255> down|up             keen ids, not virtual keys: W 63, A 41, S 59, D 44,
//                                                      Return 16, Esc 18, Space 19 (s_buttonIdKeyMap in rfg.exe)
//   <t> mouse <button 0-4> down|up                     0 left (fire), 1 right (aim)
// While a script runs, the mouse movement and the buttons it uses come from it alone (whatever the real devices do).
namespace input_script {

bool install(const std::wstring& path);

}  // namespace input_script
