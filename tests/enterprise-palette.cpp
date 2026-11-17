#include "../enterprise.cpp"
#include <cstdio>
static double observedAlpha;
void capture(void*, double, double, double, double alpha) { observedAlpha=alpha; }
int main() {
    palette = ThemeProfile{};
    double r=1,g=1,b=1;
    vectorColor(r,g,b);
    if (r != 31/255.0 || g != 41/255.0 || b != 56/255.0) return 1;
    r=g=b=0; text(r,g,b);
    if (r != 229/255.0 || g != 235/255.0 || b != 245/255.0) return 2;
    r=1;g=b=0;vectorColor(r,g,b);
    if (r != 1 || g != 0 || b != 0) return 3;
    r=0;g=128/255.0;b=0;vectorColor(r,g,b);
    if (r != 0 || g != 128/255.0 || b != 0) return 4;
    r=0;g=0;b=1;vectorColor(r,g,b);
    if (r != 0 || g != 0 || b != 1) return 5;
    r=1;g=.8;b=.2;vectorColor(r,g,b);
    if (r != 48/255.0 || g != 90/255.0 || b != 138/255.0) return 6;
    r=.75;g=.85;b=.95;vectorColor(r,g,b);
    if (r != 48/255.0 || g != 90/255.0 || b != 138/255.0) return 10;
    originalRgba=reinterpret_cast<PVOID>(&capture);active.store(true);
    hookedRgba(nullptr,1,1,1,.375);
    if (observedAlpha != .375) return 7;
    active.store(false);
    if (SetThemeProfile(nullptr) != ERROR_INVALID_PARAMETER) return 8;
    ThemeProfile invalid;invalid.version=0;
    if (SetThemeProfile(&invalid) != ERROR_INVALID_PARAMETER) return 9;
    std::puts("Palette contrast, selection, status red/green/blue, alpha and invalid payload: PASS");
    return 0;
}
