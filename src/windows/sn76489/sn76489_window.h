#pragma once

namespace SN76489Window {
void Init();
void Shutdown();
void Update();
void Render();
bool WantsKeyboardCapture();
void MuteAll();
}
