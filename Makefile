
PROJECT_NAME := Remote
SUFFIX := $(shell components/ESP32-RevK/buildsuffix)

all:	main/settings.h icons/CMakeLists.txt
	@echo Make: $(PROJECT_NAME)$(SUFFIX).bin
	@idf.py build
	@cp build/$(PROJECT_NAME).bin $(PROJECT_NAME)$(SUFFIX).bin
	@echo Done: $(PROJECT_NAME)$(SUFFIX).bin

beta:   
	-git pull
	-git submodule update --recursive
	-git commit -a
	@make set
	cp $(PROJECT_NAME)*.bin release/beta
	git commit -a -m Beta
	git push
	rsync -az release/beta/$(PROJECT_NAME)* ota.revk.uk:/var/www/ota/beta/

issue:
	-git pull
	-git commit -a
	cp -f release/beta/$(PROJECT_NAME)*.bin release
	-git commit -a -m Release
	-git push
	rsync -az release/$(PROJECT_NAME)* ota.revk.uk:/var/www/ota/

main/settings.h:     components/ESP32-RevK/revk_settings main/settings.def components/*/settings.def
	components/ESP32-RevK/revk_settings $^

components/ESP32-RevK/revk_settings: components/ESP32-RevK/revk_settings.c
	make -C components/ESP32-RevK revk_settings

%.png: %.svg
ifeq ($(shell uname),Linux)
	inkscape $< -o $@
else
	/Applications/Inkscape.app/Contents/MacOS/inkscape $< -o $@
endif
	optipng -o2 -quiet -strip all $@

icons/CMakeLists.txt:	$(patsubst %.svg,%.png,$(wildcard icons/*.svg))
	icons/make

set:    main/settings.h s3-blind s3-lcd24 s3-lcd2

s3-lcd2:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-LCD2
	@make

s3-lcd24:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-LCD24
	@make

s3-blind:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-GFXNONE
	@make

flash:
	idf.py flash

monitor:
	idf.py monitor

clean:
	idf.py clean

menuconfig:
	idf.py menuconfig

#include $(IDF_PATH)/make/project.mk

pull:
	git pull
	git submodule update --recursive

update:
	-git pull
	-git commit -a
	git submodule update --init --recursive --remote
	idf.py update-dependencies
	-git commit -a -m "Library update"
	-git push
