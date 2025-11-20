# baresip-villa


## Building instructions

	cd re
	cmake -B build
	cmake --build build
	sudo cmake --install build
	cd ..

	cd libfvad
	cmake -B build
	cmake --build build
	cd ..

	cd baresip
	git checkout immisch/aufile-src-offset
	cmake -B build
	cmake --build build
	sudo cmake --install build
	cd ..

	cd baresip-apps
	cmake -B build
	cmake --build build
	sudo cmake --install build
	cd ..

## Manual testing

	baresip/build/baresip
	/insmod menu
	/uanew <sip:villa@immisch-mbp.local>
	/auplay aufile,record.wav
	/enqueue 0 loop pause play villa/Villa/diele/dieleatm_s16.wav
	/enqueue 1 pause play villa/prototypes/door/reingehn_s16.wav
