#!/bin/sh

echo '* Runing libtoolize...'
libtoolize --install --copy --force --automake -Wall

echo '* Running aclocal...'
aclocal -I gl/m4

echo '* Runing autoconf...'
autoconf --force -Wall

echo '* Running autoheader...'
autoheader

echo '* Running automake...'
automake --add-missing --copy --force-missing

