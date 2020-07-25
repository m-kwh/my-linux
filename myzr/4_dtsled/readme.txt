如果提示nclude/linux/page-flags-layout.h:5:30: fatal error: generated/bounds.h: No such file or directory
是因为make ditclean或者make clean了没编译内核，bounds.h是编译内核过程产生的