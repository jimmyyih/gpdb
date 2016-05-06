#! /bin/bash

apt-get update
apt-get install -y apache2
apt-get install -y ssh
apt-get install -y rsync
apt-get install -y graphviz
apt-get install -y doxygen

pushd greenplum-code
doxygen doxygen/Doxyfile
ln -s `pwd` /var/www/html/greenplum-code
popd

service apache2 start
