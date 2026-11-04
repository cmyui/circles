.PHONY: build run test format

build:
	cmake -S . -B build
	cmake --build build -j8

# The game loads shaders/ and beatmaps/ relative to the working directory,
# so this runs the app's executable here rather than opening the app.
run: build
	./build/circles.app/Contents/MacOS/circles

test:
	cmake -S . -B build
	cmake --build build --target check -j8

format:
	clang-format -i src/circles/*.h src/circles/*.cc tests/*.cc tests/support/*.h
