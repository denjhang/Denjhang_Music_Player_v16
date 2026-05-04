#pragma once

namespace GigatronWindow {

void Init();
void Shutdown();
void Update();
void Render();
bool WantsKeyboardCapture();
bool IsPlaying();
void Pause();
void Resume();

}  // namespace GigatronWindow
