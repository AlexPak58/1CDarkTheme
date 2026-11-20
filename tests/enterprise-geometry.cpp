#include "../enterprise.cpp"
#include <cstdio>

template<class T> T function(HMODULE module,const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module,name));
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return 2;
    fillExtents=function<FillExtents>(module,"cairo_fill_extents");
    deviceDistance=function<TransformDistance>(module,"cairo_user_to_device_distance");
    const auto image=function<void*(*)(int,int,int)>(module,"cairo_image_surface_create");
    const auto create=function<void*(*)(void*)>(module,"cairo_create");
    const auto rectangle=function<void(*)(void*,double,double,double,double)>(module,"cairo_rectangle");
    const auto reset=function<void(*)(void*)>(module,"cairo_new_path");
    const auto scale=function<void(*)(void*,double,double)>(module,"cairo_scale");
    const auto rotate=function<void(*)(void*,double)>(module,"cairo_rotate");
    const auto destroy=function<void(*)(void*)>(module,"cairo_destroy");
    const auto destroySurface=function<void(*)(void*)>(module,"cairo_surface_destroy");
    if (!fillExtents || !deviceDistance || !image || !create || !rectangle || !reset ||
        !scale || !rotate || !destroy || !destroySurface) return 2;
    auto* surface=image(0,800,800);auto* context=create(surface);
    int checks{};
    for (double zoom : {1.,1.25,2.,4.}) {
        scale(context,zoom,zoom);
        for (double angle : {0.,.2}) {
            rotate(context,angle);
            for (const auto& shape : {std::array<double,3>{300,1,1}, {1,100,1}, {200,24,0}, {1,1,0}, {0,0,0}}) {
                reset(context);rectangle(context,10,10,shape[0],shape[1]);
                double a{},b{},c{},d{},afterA{},afterB{},afterC{},afterD{};
                fillExtents(context,&a,&b,&c,&d);
                const bool result=thinFill(context);
                fillExtents(context,&afterA,&afterB,&afterC,&afterD);
                if (result!=(shape[2]!=0 && angle==0) || a!=afterA || b!=afterB || c!=afterC || d!=afterD) {
                    std::fprintf(stderr,"geometry mismatch at zoom %.2f angle %.2f shape %.0fx%.0f\n",zoom,angle,shape[0],shape[1]);
                    return 1;
                }
                ++checks;
            }
            rotate(context,-angle);
        }
        scale(context,1/zoom,1/zoom);
    }
    destroy(context);destroySurface(surface);FreeLibrary(module);
    std::printf("PASS: %d real Cairo thin-fill geometries; path unchanged\n",checks);
    return 0;
}
