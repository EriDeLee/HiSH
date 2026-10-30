#include "package_archive.h"
#include <iostream>
int main(int argc,char** argv) {
    try {HishPackage::Require(argc==3||argc==4,"usage: archive-test APK DEST [tgz]");HishPackage::fs::create_directory(argv[2]);
        HishPackage::Extractor e(argv[2]);const auto metadata=e.Extract(HishPackage::Read(argv[1]),argc==3);e.MaterializeLinks();std::cout<<metadata;return 0;
    }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
