.PHONY: build run test format

build:
	cmake -S . -B build
	cmake --build build -j8

# Opens the app, as Finder would, so macOS asks circles itself rather than
# this terminal for permissions such as Input Monitoring. The game loads
# shaders/ and beatmaps/ from here, and prints to this terminal.
run: build
	open -n -W --stdout "$$(tty)" --stderr "$$(tty)" build/circles.app \
		--args --working-dir "$(CURDIR)"

test:
	cmake -S . -B build
	cmake --build build --target check -j8

format:
	clang-format -i src/circles/*.h src/circles/*.cc tests/*.cc tests/support/*.h
