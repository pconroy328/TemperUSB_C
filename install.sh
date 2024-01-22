#!/bin/bash
###
make clean
make

sudo cp temperusb.service /etc/systemd/system/.
sudo chmod 644 /etc/systemd/system/temperusb.service
sudo systemctl daemon-reload
sudo systemctl start temperusb
sudo systemctl status temperusb
sudo systemctl enable temperusb

