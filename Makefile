.PHONY: build run test test-metal format

build:
	cmake -S . -B build
	cmake --build build -j8

# The game loads beatmaps/, skins/ and replays/ from the working directory.
run: build
	./build/circles

test:
	cmake -S . -B build
	cmake --build build --target check -j8

test-metal:
	cmake -S . -B build
	cmake --build build --target check-metal -j8

format:
	clang-format -i src/circles/*.h src/circles/*.cc src/circles/*.mm tests/*.cc tests/support/*.h
