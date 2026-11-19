#pragma once

struct Config;

// Unattended-run support, ported from the PhysX project: block all save writes and erases, load the most recent save
// from the main menu without input, keep the player alive, quit after a given time in gameplay, and take
// screenshots at given gameplay seconds (and scripted input, test/input_script.h). Driven from the main-loop frame
// callback.
namespace automation {

bool install(const Config& cfg);

}  // namespace automation
