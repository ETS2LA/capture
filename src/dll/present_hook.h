#pragma once

#include <d3d11.h>
#include <dxgi.h>

bool StartPresentHook();
void StopPresentHook(bool process_detach = false);