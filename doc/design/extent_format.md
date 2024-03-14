# File management

We changed how the file names on the media are written between version 1.95 and
2.0. It was changed from "oid.version.layout\_tag.object\_uuid" to
"oid.extent\_uuid".

The goal is to make the name more generic by removing the layout tag, as it
should be chosen and set by the io module, without regard to the layout used.

Moreover, this change keeps the name unique as we use the extent uuid, which is
generated by standard uuid-generation methods for each extent.

# Extended attributes

Other information have been added to the extended attributes of each extent.
These include:
 - The UUID, size, version, user metadata, MD5 and XXH128 of the object the
 extent is a part of,
 - The extent's offset with regard to the object data,
 - The layout used to write the object.

Each of these attributes is set by the I/O module responsible for writing the
extent, and they are considered as "common" attributes.

Meanwhile, the layout module can write additionnal attributes, which it can use
to specify how the object is written. For instance, the RAID1 layout sets
these attributes to each extent:
 - The object's replica count,
 - The extent's index amongst all of the object's extents.

Finally, the layout specific attributes are prepended by the name of the layout
used, i.e. "raid1.extent\_index" for example.