#pragma once
#include <string>
#include <vector>
bool ApplyRegions(const std::vector<bool>& unblocked, std::wstring& errOut);
bool DetectUnblockedRegions();
