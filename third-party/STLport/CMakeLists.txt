include(ExternalProject)
ExternalProject_Add(
   STLport
   PREFIX .
   DOWNLOAD_DIR $ENV{EMBOX_ROOT}/download
   URL http://download.sourceforge.net/project/stlport/STLport/STLport-5.2.1/STLport-5.2.1.tar.bz2
   URL_MD5 a8341363e44d9d06a60e03215b38ddde
   UPDATE_COMMAND ""
   PATCH_COMMAND
      cd $ENV{EMBOX_ROOT}/third-party/STLport/tree &&
      bash -c "cp -l -u -f --parents -t <SOURCE_DIR>/ `find . -not -path '*/.*' -and -type f`" &&
      patch -p1 -d <SOURCE_DIR> -N --input=$ENV{EMBOX_ROOT}/third-party/STLport/patch.txt
   BUILD_IN_SOURCE 1
   LOG_CONFIGURE 1
   CONFIGURE_COMMAND
      <SOURCE_DIR>/configure
         --prefix=<INSTALL_DIR>
         --target=embox
         ""--with-extra-cflags=$ENV{EMBOX_DERIVED_CFLAGS}""
         ""--with-extra-cxxflags=$ENV{EMBOX_DERIVED_CXXFLAGS}""
         --with-cc=$ENV{EMBOX_DERIVED_CC}
         --with-cxx=$ENV{EMBOX_DERIVED_CXX}
         --enable-static
         --disable-shared
         $ENV{EMBOX_DERIVED_RTTI_FLAG}
         --with-static-rtl
   #	BUILD_COMMAND
   LOG_BUILD 1
)
