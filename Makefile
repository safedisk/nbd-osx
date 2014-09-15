all: 
	xcodebuild
	kextlibs build/Release/nbd.kext

debug:
	xcodebuild -configuration Debug
	kextlibs build/Debug/nbd.kext

clean:
	xcodebuild clean
	xcodebuild -configuration Debug clean

load:
	sudo kextload /tmp/nbd.kext

unload:
	sudo kextunload /tmp/nbd.kext

reload:
	sudo kextunload /tmp/nbd.kext || true
	sudo cp -R build/${CFG}/nbd.kext /tmp
	kextutil -tn /tmp/nbd.kext
	sudo kextload /tmp/nbd.kext

# https://stackoverflow.com/questions/11487596/making-os-x-installer-packages-like-a-pro-xcode4-developer-id-mountain-lion-re
