#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <cstdio>
#include <array>
int main(int argc, char** argv)
{
    std::array<wchar_t,32768> name{};
    const auto count=GetModuleFileNameW(nullptr,name.data(),static_cast<DWORD>(name.size()));
    if(!count || count>=name.size()) return 1;
    const auto library=std::filesystem::path(name.data()).parent_path()/L"pvz_dfu.dll";
    HMODULE handle=LoadLibraryExW(library.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!handle){std::fprintf(stderr,"Cannot load data/pvz_dfu.dll (Win32 error %lu)\n",GetLastError());return 1;}
    using Entry=int(*)(int,char**) noexcept;
    const auto entry=reinterpret_cast<Entry>(GetProcAddress(handle,"pvz_dfu_cli_v1"));
    if(!entry){std::fputs("DFU converter ABI missing\n",stderr);FreeLibrary(handle);return 1;}
    const auto result=entry(argc,argv);
    FreeLibrary(handle);
    return result;
}
