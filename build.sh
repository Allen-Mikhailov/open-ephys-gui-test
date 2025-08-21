cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .
cd Build
cmake -G "Unix Makefiles" ..
make
make install
