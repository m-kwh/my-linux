cmd_drivers/video/built-in.o :=  arm-poky-linux-gnueabi-ld -EL    -r -o drivers/video/built-in.o drivers/video/hdmi.o drivers/video/console/built-in.o drivers/video/logo/built-in.o drivers/video/backlight/built-in.o drivers/video/fbdev/built-in.o drivers/video/display_timing.o drivers/video/videomode.o drivers/video/of_display_timing.o drivers/video/of_videomode.o 
