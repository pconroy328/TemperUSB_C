#!/bin/bash
###
make clean
make
mv ./dist/Debug/GNU-Linux/temperusb_c ./dist/Debug/GNU-Linux/temperusb

sudo cp temperusb.service /etc/systemd/system/.
sudo chmod 644 /etc/systemd/system/temperusb.service
sudo systemctl daemon-reload
sudo systemctl start temperusb
sudo systemctl status temperusb
sudo systemctl enable temperusb

