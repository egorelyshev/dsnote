set(mbrola_source_url "https://github.com/numediart/MBROLA/archive/refs/tags/3.3.tar.gz")
set(mbrola_checksum "c01ded2c0a05667e6df2439c1c02b011a5df2bfdf49e24a524630686aea2b558")

set(espeak_source_url "https://github.com/rhasspy/espeak-ng/archive/8593723f10cfd9befd50de447f14bf0a9d2a14a4.zip")
set(espeak_checksum "cc8092f23a28ccd79b1c5e62984a4c4ac1959d2d0b8193ac208d728c620bd5ed")

ExternalProject_Add(mbrola
    SOURCE_DIR ${external_dir}/mbrola
    BINARY_DIR ${PROJECT_BINARY_DIR}/external/mbrola
    INSTALL_DIR ${PROJECT_BINARY_DIR}/external
    URL ${mbrola_source_url}
    URL_HASH SHA256=${mbrola_checksum}
    CONFIGURE_COMMAND cp -r --no-target-directory <SOURCE_DIR> <BINARY_DIR>
    BUILD_COMMAND ${MAKE}
    BUILD_ALWAYS False
    INSTALL_COMMAND mkdir -p ${external_bin_dir} && cp <BINARY_DIR>/Bin/mbrola ${external_bin_dir}
)

ExternalProject_Add(espeak
    SOURCE_DIR ${external_dir}/espeak
    BINARY_DIR ${PROJECT_BINARY_DIR}/external/espeak
    INSTALL_DIR ${PROJECT_BINARY_DIR}/external
    URL ${espeak_source_url}
    URL_HASH SHA256=${espeak_checksum}
    CONFIGURE_COMMAND cp -r --no-target-directory <SOURCE_DIR> <BINARY_DIR> &&
        <BINARY_DIR>/autogen.sh &&
        <BINARY_DIR>/configure --prefix=<INSTALL_DIR> --with-pic
        --with-pcaudiolib=no --with-sonic=no --with-speechplayer=no
        --with-mbrola=yes --enable-static --with-extdict-ru
    BUILD_COMMAND ${MAKE}
    BUILD_ALWAYS False
    INSTALL_COMMAND make DESTDIR=/ install
)

ExternalProject_Add_StepDependencies(espeak configure mbrola)

list(APPEND deps_libs "${external_lib_dir}/libespeak-ng.a")
list(APPEND deps espeak mbrola)
