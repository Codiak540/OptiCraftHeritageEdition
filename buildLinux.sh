cmake -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMC_LOG_LEVEL=2 \
  "-DCMAKE_CXX_FLAGS=-I$PWD/src/net/minecraft/src"
cmake --build build/release