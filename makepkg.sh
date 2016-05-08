#!/bin/sh

# make sure git repo is clean
if [ "$(git clean -n)" != "" ]; then
	echo "Please run 'git clean -f' first!"
	exit 1
fi

# cleanup git clean leftovers
if [ -d src/.deps ] || [ -d gl/.libs ] || [ -d autom4te.cache ]; then
	echo "* Cleaning 'git clean' leftovers"
	rm -rf src/.deps
	rm -rf gl/.libs
	rm -rf autom4te.cache
fi

# generate configure script
./autogen.sh

# version
version="0.8"

# pkg file
pkg_file="$(pwd)/packages/avmount-${version}.tar.xz"

# create directories
rm -rf /tmp/avmount-${version}
mkdir -p /tmp/avmount-${version}
mkdir -p $(pwd)/packages

# copy files
echo "* Copying project files to temp location..."
find . \
	-maxdepth 1 -mindepth 1 \
	! -name 'showmem' \
	! -name 'autogen.sh' \
	! -name 'packages' \
	! -name '.git' \
	-exec cp -r '{}' /tmp/avmount-${version} \;

# create tarball
echo "* Creating tarball..."
cd /tmp
tar --xz -cf ${pkg_file} ./avmount-${version}
echo "* Tarball created at ${pkg_file}."

# cleanup
echo "* Cleaning up..."
rm -rf /tmp/avmount-${version}
