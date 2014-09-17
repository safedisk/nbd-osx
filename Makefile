all: buse
	xcodebuild
	kextlibs build/Release/nbd.kext

debug: buse
	xcodebuild -configuration Debug
	kextlibs build/Debug/nbd.kext

clean:
	xcodebuild clean
	xcodebuild -configuration Debug clean

load:
	kextload /tmp/nbd.kext

unload:
	kextunload /tmp/nbd.kext

reload:
	kextunload /tmp/nbd.kext || true
	cp -R build/Debug/nbd.kext /tmp
	kextutil -tn /tmp/nbd.kext
	kextload /tmp/nbd.kext

buse: client/main.cpp
	g++ -std=c++11 -Inbd -O0 -o buse client/main.cpp
