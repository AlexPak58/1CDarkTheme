#include "../enterprise_gradient.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

using namespace enterprise_gradient;
static int checks, calls, freed;
static void* observed;
static void* borrowed;
static bool retain=true;
static SetSource realSetSource;
static unsigned (*references)(void*);
struct Key { int unused; };
static Key key{};
static int (*userData)(void*,const Key*,void*,void(*)(void*));
static constexpr DWORD incoming=0x12345678, outgoing=0x87654321;
static void check(bool ok,const char* message) {
    ++checks;
    if (!ok) { std::printf("FAIL %d: %s\n",checks,message); std::exit(1); }
}
static void released(void*) { ++freed; SetLastError(42); }
static void capture(void* cr,void* pattern) {
    check(GetLastError()==incoming,"original incoming LastError");
    ++calls; observed=pattern;
    if (pattern!=borrowed) {
        check(references(pattern)==1,"new clone has one owned reference");
        check(userData(pattern,&key,&key,released)==0,"clone lifetime observer");
    }
    if (retain) realSetSource(cr,pattern);
    SetLastError(outgoing);
}
static void invert(double& r,double& g,double& b) { r=1-r;g=1-g;b=1-b; }
static void noop(double&,double&,double&) {}
static void invalid(double& r,double&,double&) { r=std::numeric_limits<double>::quiet_NaN(); }
static void throwing(double&,double&,double&) { throw std::runtime_error("test"); }
static void* (*createImage)(int,int,int);
static void* recolorImage(void*) { return createImage(0,17,17); }
static void select(void* context,void* source,Transform transform,bool cloned,Transform solid = nullptr,
                   TransformSurface image = nullptr) {
    borrowed=source;calls=0;observed=nullptr;SetLastError(incoming);
    setSource(capture,context,source,transform,solid,image);
    check(GetLastError()==outgoing,"original outgoing LastError survives cleanup");
    check(calls==1,"original called exactly once");
    check(cloned ? observed!=source : observed==source,"selected expected source");
}
template<class T> T function(HMODULE module,const char* name) {
    const auto result=reinterpret_cast<T>(GetProcAddress(module,name));
    check(result!=nullptr,name); return result;
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    // No Cairo is initialized: the wrapper is a transparent fallback.
    retain=false; select(nullptr,nullptr,invert,false);
    SetLastError(incoming); check(!initialize(nullptr),"reject absent module");
    check(GetLastError()==incoming,"initialize failure LastError");
    SetLastError(incoming);check(!initialize(GetModuleHandleW(L"kernel32.dll")),"reject foreign module hash before publication");
    check(GetLastError()==incoming,"hash rejection LastError");
    const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    check(module!=nullptr,"load actual Cairo fixture");
    SetLastError(incoming);check(initialize(module),"initialize exact pinned Cairo");
    check(GetLastError()==incoming,"initialize success LastError");
    check(initialize(module),"repeat initialization");
    check(!initialize(GetModuleHandleW(L"kernel32.dll")),"reject different module");
    const auto& api=*detail::published.load();
    realSetSource=function<SetSource>(module,"cairo_set_source");
    references=function<unsigned(*)(void*)>(module,"cairo_pattern_get_reference_count");
    userData=function<decltype(userData)>(module,"cairo_pattern_set_user_data");
    const auto image=function<void*(*)(int,int,int)>(module,"cairo_image_surface_create");
    createImage=image;
    const auto create=function<void*(*)(void*)>(module,"cairo_create");
    const auto destroy=function<void(*)(void*)>(module,"cairo_destroy");
    const auto destroySurface=function<void(*)(void*)>(module,"cairo_surface_destroy");
    const auto source=function<void*(*)(void*)>(module,"cairo_get_source");
    const auto solid=function<void*(*)(double,double,double)>(module,"cairo_pattern_create_rgb");
    auto* surface=image(0,64,64);auto* context=create(surface);retain=true;
    for (int type : {2,3}) {
        auto* original=type==2 ? api.createLinear(1,2,17,23) : api.createRadial(2,3,1,7,8,9);
        api.addStop(original,.2,.9,.85,.8,.35);api.addStop(original,.8,.2,.3,.4,.9);
        const Matrix matrix{2,.25,.5,3,4,5};
        api.setMatrix(original,&matrix);api.setFilter(original,3);api.setExtend(original,2);
        for (int cycle=0;cycle<8;++cycle) {
            const auto priorFreed=freed;
            select(context,original,invert,true);
            auto* clone=source(context);
            check(clone==observed && references(clone)==1,"context retains clone after owned release");
            check(references(original)==1,"borrowed original reference unchanged");
            check(api.type(clone)==type,"gradient kind preserved");
            Matrix actual{};api.matrix(clone,&actual);
            check(std::memcmp(&actual,&matrix,sizeof(matrix))==0,"matrix preserved");
            check(api.filter(clone)==3 && api.extend(clone)==2,"filter and extend preserved");
            api.matrix(original,&actual);
            check(std::memcmp(&actual,&matrix,sizeof(matrix))==0 && api.filter(original)==3 && api.extend(original)==2,"original metadata immutable");
            std::array<double,6> before{},after{};
            if (type==2) {
                api.linear(original,&before[0],&before[1],&before[2],&before[3]);
                api.linear(clone,&after[0],&after[1],&after[2],&after[3]);
            } else {
                api.radial(original,&before[0],&before[1],&before[2],&before[3],&before[4],&before[5]);
                api.radial(clone,&after[0],&after[1],&after[2],&after[3],&after[4],&after[5]);
            }
            check(before==after,"geometry preserved");
            int count{};check(api.count(clone,&count)==0 && count==2,"stop count preserved");
            for (int i=0;i<2;++i) {
                double o{},r{},g{},b{},a{},co{},cr{},cg{},cb{},ca{};
                api.stop(original,i,&o,&r,&g,&b,&a);api.stop(clone,i,&co,&cr,&cg,&cb,&ca);
                check(o==co && a==ca && cr==1-r && cg==1-g && cb==1-b,"RGB transformed; alpha and offset preserved");
                check(o==(i==0?.2:.8) && r==(i==0?.9:.2) && g==(i==0?.85:.3) && b==(i==0?.8:.4) && a==(i==0?.35:.9),"cached original immutable");
            }
            // Disabled callers bypass the wrapper, reusing the unchanged cached source.
            realSetSource(context,original);
            check(source(context)==original && freed==priorFreed+1,"off restores original and frees clone once");
        }
        select(context,original,nullptr,false);select(context,original,noop,false);
        select(context,original,invalid,false);select(context,original,throwing,false);
        auto* black=solid(0,0,0); realSetSource(context,black); api.destroy(black);
        api.destroy(original);
    }
    auto* plain=solid(.2,.3,.4);select(context,plain,invert,false);api.destroy(plain);
    auto* highlight=api.createSolid(.25,.5,.75,.625);
    select(context,highlight,noop,true,invert);
    double red{},green{},blue{},alpha{};
    check(api.rgba(source(context),&red,&green,&blue,&alpha)==0 &&
          red==.75 && green==.5 && blue==.25 && alpha==.625,"solid clone preserves alpha");
    check(api.rgba(highlight,&red,&green,&blue,&alpha)==0 &&
          red==.25 && green==.5 && blue==.75 && alpha==.625,"cached solid pattern stays unchanged");
    select(context,highlight,invert,false,noop);
    api.destroy(highlight);
    auto* raster=api.createForSurface(surface);
    const Matrix imageMatrix{1.25,0,0,1.5,2,3};
    api.setMatrix(raster,&imageMatrix);api.setFilter(raster,4);api.setExtend(raster,1);
    select(context,raster,noop,true,nullptr,recolorImage);
    const auto replacement=source(context);
    void* originalPixels{}; void* replacedPixels{};
    Matrix observedMatrix{};api.matrix(replacement,&observedMatrix);
    check(!api.surface(raster,&originalPixels) && originalPixels==surface &&
          !api.surface(replacement,&replacedPixels) && replacedPixels && replacedPixels!=surface,
          "image pattern owns distinct themed pixels and original remains intact");
    check(std::memcmp(&observedMatrix,&imageMatrix,sizeof(imageMatrix))==0 &&
          api.filter(replacement)==4 && api.extend(replacement)==1,"image matrix, filter and extend preserved");
    api.destroy(raster);
    for (int count : {0,128,129}) {
        auto* pattern=api.createLinear(0,0,1,1);
        for (int i=0;i<count;++i) api.addStop(pattern,static_cast<double>(i)/128,.2,.3,.4,.5);
        select(context,pattern,invert,count==128);api.destroy(pattern);
    }
    // A non-retaining original releases the owned clone immediately; cleanup must preserve LastError.
    retain=false;
    auto* temporary=api.createLinear(0,0,1,1);api.addStop(temporary,0,.2,.3,.4,.5);
    const auto priorFreed=freed;select(nullptr,temporary,invert,true);
    check(freed==priorFreed+1 && references(temporary)==1,"unretained clone freed and original stays owned");
    api.destroy(temporary);
    // Non-finite and error fixtures must fail open without passing them to a real context.
    retain=false;
    auto* nonfinite=api.createLinear(0,0,std::numeric_limits<double>::infinity(),1);
    api.addStop(nonfinite,0,.2,.3,.4,.5);
    select(nullptr,nonfinite,invert,false);api.destroy(nonfinite);
    auto* error=api.createRadial(0,0,-1,1,1,2);
    select(nullptr,error,invert,false);api.destroy(error);
    destroy(context);destroySurface(surface);
    std::printf("PASS %d real Cairo gradient checks\n",checks);
    FreeLibrary(module);return 0;
}
