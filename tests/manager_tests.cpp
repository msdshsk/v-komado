#include "../app/core.h"
#include <iostream>
#include <fstream>
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);std::cout<<"PASS "<<text<<std::endl;}
template<class F> bool rejects(F action){try{action();return false;}catch(const svd::Error&){return true;}}
int wmain(int argc,wchar_t** argv){
  try {
    check(svd::vdd_hardware({L"ROOT\\MTTVDD"}),"VDD hardware ID matches case-insensitively");
    check(!svd::vdd_hardware({L"PCI\\VEN_10DE",L"ROOT\\MttVDDExtra"}),"physical GPU and similar IDs rejected");
    check(svd::valid_oem_inf(L"oem42.inf"),"published INF filename accepted");
    for(auto text:{L"oem.inf",L"../oem1.inf",L"oem1.inf /force",L"C:\\Windows\\oem1.inf",L"oem1x.inf"})check(!svd::valid_oem_inf(text),"arbitrary INF/path rejected");
    svd::State state;state.exists=true;state.id=L"ROOT\\DISPLAY\\0000";
    svd::Device a;a.id=state.id;svd::Device b;b.id=L"ROOT\\DISPLAY\\0001";
    check(svd::select_device(state,{a}).id==a.id,"recorded device selected");
    check(rejects([&]{svd::select_device(state,{});}),"missing device rejected");
    check(rejects([&]{svd::select_device(state,{b});}),"replacement device rejected");
    check(rejects([&]{svd::select_device(state,{a,b});}),"multiple VDD devices rejected");
    check(rejects([&]{svd::select_device(svd::State{},{a});}),"unrecorded installation rejected");
    if(argc!=2)throw std::runtime_error("supply verified driver directory");
    svd::validate_payload(argv[1]);check(true,"real driver payload hashes verified");
    auto temporary=svd::fs::temp_directory_path()/(L"SharedVD-test-"+std::to_wstring(GetCurrentProcessId()));
    svd::fs::create_directory(temporary);
    for(auto name:{L"MttVDD.inf",L"MttVDD.dll",L"mttvdd.cat"})svd::fs::copy_file(svd::fs::path(argv[1])/name,temporary/name);
    {std::ofstream corrupt(temporary/L"MttVDD.inf",std::ios::app);corrupt<<"modified";}
    check(rejects([&]{svd::validate_payload(temporary);}),"modified driver refused before any installation");
    for(auto name:{L"MttVDD.inf",L"MttVDD.dll",L"mttvdd.cat"})svd::fs::remove(temporary/name);
    svd::fs::remove(temporary);
    std::cout<<"manager_tests=PASS (no driver or registry changes)"<<std::endl;return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
}
