v=$(cat $(dirname $0)/version)
t="Debian $v $(date +\(%F\))"
cp config .config
make deb-pkg BUILD_TOOLS=1 KDEB_PKGVERSION=$v KERNELRELEASE=4.19 LOCALVERSION=_$v KBUILD_BUILD_TIMESTAMP="$t" -j 38
