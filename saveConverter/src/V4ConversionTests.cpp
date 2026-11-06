// Offline conversion coverage for the supported version family only.
#include "LevelSaveConverter.cpp"
#include <chrono>
#include <iostream>
namespace {
using namespace PvzDat64;
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F> void Reject(F operation,const char* why){try{operation();}catch(const std::runtime_error&){return;}throw std::runtime_error(why);}
void Save(const std::filesystem::path& path,const std::vector<Byte>& bytes){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());Require(bool(out),"Cannot save test input");}
void Check(const std::filesystem::path& original,const std::filesystem::path& directory){
    const auto source=ReadFileFully(original);
    std::uint32_t echo=0;
    const auto parsedOriginal=Parse(source,Format::Legacy1073Dat,echo);
    const auto originalHash=Crc32(source);
    std::vector<Byte> current;
    for(const auto format:{Format::Legacy1073Dat,Format::Dat64V1,Format::Dat64V2,Format::Dat64V4}){
        auto expected=parsedOriginal;
        PrepareAppearanceOutput(expected,format);
        PrepareV7Output(expected,format);
        const auto wire=WriteFileImage(expected,format,echo);
        auto otherEcho=echo;
        const auto parsed=Parse(wire,format,otherEcho);
        Require(parsed==expected,"Supported grammar changed canonical data on roundtrip");
        Require(WriteFileImage(parsed,format,otherEcho)==wire,"Supported grammar is not byte-idempotent");
        const auto input=directory/(std::string("input-")+std::to_string(int(format))+".dat64");
        const auto output=directory/(std::string("current-")+std::to_string(int(format))+".dat64");
        Save(input,wire);
        ConversionOptions options;options.mTargetFormat=Format::Dat64V4;
        ConvertFile(input,output,options);
        const auto result=ReadFileFully(output);
        Require(DetectFormat(output)==Format::Dat64V4 && LevelSaveFormat::IsCurrentDat64Header(result),"Conversion did not produce current v4");
        auto resultEcho=echo;const auto resultDoc=Parse(result,Format::Dat64V4,resultEcho);
        Require(WriteFileImage(resultDoc,Format::Dat64V4,resultEcho)==result,"Converted v4 is not byte-idempotent");
        if(format==Format::Dat64V4)current=wire;
    }
    Require(ReadU32At(current,8)==4 && ReadU32At(current,40)==0 && ReadU32At(current,36)==0x10745,"Wrong v4 header identity");
    auto edited=current;WriteU32At(edited,32,0x12345678);
    Require(Parse(edited,Format::Dat64V4,echo)==Parse(current,Format::Dat64V4,echo),"CRC slot was incorrectly enforced");
    const auto defaultOutput=directory/"default-v4.dat64";
    Require(ConvertFile(original,defaultOutput).mTargetFormat==Format::Dat64V4 &&
        DetectFormat(defaultOutput)==Format::Dat64V4,"Default original import did not select v4");
    const auto currentInput=directory/"current-input.dat64";Save(currentInput,current);
    for(const auto format:{Format::Legacy1073Dat,Format::Dat64V1,Format::Dat64V2}){
        const auto output=directory/(std::string("export-")+std::to_string(int(format))+".dat64");
        ConversionOptions options;options.mTargetFormat=format;options.mRoundTimeToNearestTick=true;
        ConvertFile(currentInput,output,options);
        auto expected=parsedOriginal;PrepareAppearanceOutput(expected,format);PrepareV7Output(expected,format);
        Require(Parse(ReadFileFully(output),format,echo)==expected,"v4 legacy export changed original canonical state");
    }
    auto fractional=Parse(current,Format::Dat64V4,echo);
    Require(!fractional.mNanoseconds.empty(),"Fixture has no nanosecond values");
    ++fractional.mNanoseconds.front();
    const auto fractionalInput=directory/"fractional-v4.dat64";
    Save(fractionalInput,WriteFileImage(fractional,Format::Dat64V4,echo));
    const auto fractionalOutput=directory/"fractional-v1.dat64";
    ConversionOptions strict;strict.mTargetFormat=Format::Dat64V1;
    Reject([&]{ConvertFile(fractionalInput,fractionalOutput,strict);},"Strict old export accepted fractional nanoseconds");
    Require(!std::filesystem::exists(fractionalOutput),"Rejected fractional export created output");
    strict.mRoundTimeToNearestTick=true;
    ConvertFile(fractionalInput,fractionalOutput,strict);
    Require(DetectFormat(fractionalOutput)==Format::Dat64V1,"Explicit nearest export failed");
    const auto rejectedInput=directory/"unsupported-v3.dat64";
    const auto untouched=directory/"untouched-output.dat64";
    const auto absent=directory/"must-not-exist.dat64";
    const std::vector<Byte> sentinel{1,3,5,7};
    {
        auto retired=current;WriteU32At(retired,36,0x10744);
        Save(rejectedInput,retired);Save(untouched,sentinel);
        Require(!LevelSaveFormat::IsSupportedDat64Header(retired),"Retired v4 draft schema accepted");
        Reject([&]{ConvertFile(rejectedInput,untouched);},"Conversion accepted retired v4 draft");
        Require(ReadFileFully(untouched)==sentinel,"Retired v4 rejection modified existing output");
        Reject([&]{ConvertFile(rejectedInput,absent);},"Retired v4 created output");
        Require(!std::filesystem::exists(absent),"Retired v4 rejection created output");
    }
    for(unsigned int minor=0;minor<=7;++minor)for(unsigned int schema=0x10733;schema<=0x10745;++schema){
        auto rejected=current;WriteU32At(rejected,8,3);WriteU32At(rejected,40,minor);WriteU32At(rejected,36,schema);
        Require(!LevelSaveFormat::IsSupportedDat64Header(rejected) && !LevelSaveFormat::IsCurrentDat64Header(rejected),"Shared header accepted v3");
        Save(rejectedInput,rejected);Save(untouched,sentinel);
        Reject([&]{DetectFormat(rejectedInput);},"Auto detection accepted v3");
        Reject([&]{ConvertFile(rejectedInput,untouched);},"Conversion accepted v3");
        Require(ReadFileFully(untouched)==sentinel,"Rejected conversion modified existing output");
        Reject([&]{ConvertFile(rejectedInput,absent);},"Conversion accepted v3 into absent path");
        Require(!std::filesystem::exists(absent),"Rejected conversion created output");
    }
    Require(Crc32(ReadFileFully(original))==originalHash && ReadFileFully(original)==source,"Input fixture changed");
}
}
int main(int argc,char** argv){try{
    Require(argc==2,"Pass original 1073 DAT fixture");
    const auto directory=std::filesystem::temp_directory_path()/("pvz-v4-conversion-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    Check(argv[1],directory);
    std::cout<<"PASS legacy/v1/v2/v4 roundtrips, current default/import/export, v4 header and CRC independence, all v3 headers rejected without output writes. Evidence: "<<directory.string()<<'\n';
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
