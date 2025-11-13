VERSION=1.3.0

check:
	rpmlint -i dist/paho-c.spec

rpm-prep:
	mkdir -p ${HOME}/rpmbuild/SOURCES/
	tar --transform="s/\./paho-c-${VERSION}/" -cf ${HOME}/rpmbuild/SOURCES/v${VERSION}.tar.gz --exclude=./build.paho --exclude=.git --exclude=*.bz ./ --gzip

rpm: rpm-prep
	rpmbuild -ba dist/paho-c.spec
