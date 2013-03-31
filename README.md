apns-fake-server
================

for build:
bjam link=static

for testing:
perl -e 'print pack("CNNnH64n", 1, 1234, 0, 32, "ff"x32, 4), "test" for (0..9);' | openssl s_client -connect localhost:12195
