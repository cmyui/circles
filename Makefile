.PHONY: build run test format

build:
	cmake -S . -B build
	cmake --build build -j8

# The game loads shaders/ and beatmaps/ relative to the working directory.
run: build
	./build/circles

test:
	cmake -S . -B build
	cmake --build build --target check -j8

format:
	clang-format -i src/circles/*.h src/circles/*.cc tests/*.cc tests/support/*.h
