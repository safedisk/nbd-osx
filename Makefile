#  osx-nbd
#  Copyright (C) 2014 Frank Laub
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU Affero General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Affero General Public License for more details.
#
#  You should have received a copy of the GNU Affero General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.

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
