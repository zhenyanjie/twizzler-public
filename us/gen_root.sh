#!/usr/bin/env bash

set -e
set -u

export SYSROOT=$(pwd)/projects/$PROJECT/build/us/sysroot
export OBJROOT=$(pwd)/projects/$PROJECT/build/us/objroot
export KEYROOT=$(pwd)/projects/$PROJECT/build/us/keyroot

for ent in $(find -L projects/$PROJECT/build/us/sysroot | cut -d'/' -f6- | grep -v '\.data$'); do
	if [[ -f $SYSROOT/$ent ]]; then
		target=$OBJROOT/${ent//\//_}.obj
		key=$KEYROOT/${ent//\//_}.pubkey.obj
		extra=
		perms=rxh
		if [[ -f $key ]]; then
			extra="-k $(./projects/$PROJECT/build/utils/objstat -i $key)"
			perms=rh
		fi

		if [[ -L $SYSROOT/$ent ]]; then
			link=$(readlink $SYSROOT/$ent)
			echo $ent'*SYM*'$link
		else
			if [[ -f $SYSROOT/$ent.data ]]; then
				target_data=$OBJROOT/${ent//\//_}.data.obj
				projects/$PROJECT/build/utils/file2obj -i $SYSROOT/$ent.data -o $target_data -p rh
				id_data=$(projects/$PROJECT/build/utils/objstat -i $target_data)
				ln $target_data $OBJROOT/$id_data
				echo $ent.data'*'$id_data

				projects/$PROJECT/build/utils/file2obj -i $SYSROOT/$ent -o $target -p $perms -f 1:RWD:$id_data $extra
				id=$(projects/$PROJECT/build/utils/objstat -i $target)
				ln $target $OBJROOT/$id
				echo $ent'*'$id
			else
				projects/$PROJECT/build/utils/file2obj -i $SYSROOT/$ent -o $target -p $perms $extra
				id=$(projects/$PROJECT/build/utils/objstat -i $target)
				ln $target $OBJROOT/$id
				echo $ent'*'$id
			fi
		fi
	fi
done

