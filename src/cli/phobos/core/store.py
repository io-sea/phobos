#!/usr/bin/env python3

#
#  All rights reserved (c) 2014-2024 CEA/DAM.
#
#  This file is part of Phobos.
#
#  Phobos is free software: you can redistribute it and/or modify it under
#  the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation, either version 2.1 of the License, or
#  (at your option) any later version.
#
#  Phobos is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public License
#  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
#

"""
High level interface to access the object store.
"""

import errno
import logging
import os

from collections import namedtuple
from ctypes import (byref, c_bool, c_char_p, c_int, c_ssize_t, c_void_p, cast,
                    CFUNCTYPE, pointer, POINTER, py_object, Structure, Union)

from phobos.core.ffi import LIBPHOBOS, DeprecatedObjectInfo, ObjectInfo, Tags
from phobos.core.const import (PHO_XFER_OBJ_REPLACE, PHO_XFER_OBJ_BEST_HOST, # pylint: disable=no-name-in-module
                               PHO_XFER_OBJ_HARD_DEL,
                               PHO_XFER_OP_GET, PHO_XFER_OP_GETMD,
                               PHO_XFER_OP_PUT, PHO_RSC_INVAL, str2rsc_family)
from phobos.core.dss import dss_sort

ATTRS_FOREACH_CB_TYPE = CFUNCTYPE(c_int, c_char_p, c_char_p, c_void_p)

class PhoAttrs(Structure): # pylint: disable=too-few-public-methods
    """Embedded hashtable, typically exposed as python dict here."""
    _fields_ = [
        ('attr_set', c_void_p)
    ]

    def __init__(self):
        super().__init__()
        self.attr_set = 0

def attrs_as_dict(attrs):
    """Return a python dictionary containing the attributes"""
    def inner_attrs_mapper(key, val, c_data):
        """
        Not for direct use.
        Add attribute of a PhoAttr structure into a python dict.
        """
        data = cast(c_data, POINTER(py_object)).contents.value
        data[key] = val
        return 0
    res = {}
    callback = ATTRS_FOREACH_CB_TYPE(inner_attrs_mapper)
    c_res = cast(pointer(py_object(res)), c_void_p)
    LIBPHOBOS.pho_attrs_foreach(byref(attrs), callback, c_res)
    res = {k.decode('utf-8'): v.decode('utf-8') for k, v in res.items()}
    return res

class XferPutParams(Structure): # pylint: disable=too-few-public-methods, too-many-instance-attributes
    """Phobos PUT parameters of the XferDescriptor."""
    _fields_ = [
        ("size", c_ssize_t),
        ("family", c_int),
        ("_grouping", c_char_p),
        ("_library", c_char_p),
        ("_layout_name", c_char_p),
        ("lyt_params", PhoAttrs),
        ("tags", Tags),
        ("_alias", c_char_p),
        ("overwrite", c_bool),
    ]

    def set_lyt_params(self, val):
        """Insert key/values attributes in Xfer's layout params"""
        if val:
            for k, v in val.items():
                LIBPHOBOS.pho_attr_set(byref(self.lyt_params),
                                       str(k).encode('utf8'),
                                       str(v).encode('utf8'))

    def __init__(self, put_params):
        super().__init__()
        self.size = -1
        self.grouping = put_params.grouping
        self.library = put_params.library
        self.layout_name = put_params.layout
        self.set_lyt_params(put_params.lyt_params)
        self.tags = Tags(put_params.tags)
        self.alias = put_params.alias
        self.overwrite = put_params.overwrite

        if put_params.family is None:
            self.family = PHO_RSC_INVAL
        else:
            self.family = str2rsc_family(put_params.family)

    @property
    def grouping(self):
        """Wrapper to get grouping"""
        return self._grouping.decode('utf-8') if self._grouping else None

    @grouping.setter
    def grouping(self, val):
        """Wrapper to set grouping"""
        # pylint: disable=attribute-defined-outside-init
        self._grouping = val.encode('utf-8') if val else None

    @property
    def library(self):
        """Wrapper to get library"""
        return self._library.decode('utf-8') if self._library else None

    @library.setter
    def library(self, val):
        """Wrapper to set library"""
        # pylint: disable=attribute-defined-outside-init
        self._library = val.encode('utf-8') if val else None

    @property
    def layout_name(self):
        """Wrapper to get layout_name"""
        return self._layout_name.decode('utf-8') if self._layout_name else None

    @layout_name.setter
    def layout_name(self, val):
        """Wrapper to set layout_name"""
        # pylint: disable=attribute-defined-outside-init
        self._layout_name = val.encode('utf-8') if val else None

    @property
    def alias(self):
        """Wrapper to get alias"""
        return self._alias.decode('utf-8') if self._alias else None

    @alias.setter
    def alias(self, val):
        """Wrapper to set alias"""
        # pylint: disable=attribute-defined-outside-init
        self._alias = val.encode('utf-8') if val else None

class PutParams(namedtuple('PutParams',
                           'alias family grouping library layout lyt_params '
                           'overwrite tags')):
    """
    Transition data structure for put parameters between
    the CLI and the XFer data structure.
    """
    __slots__ = ()
PutParams.__new__.__defaults__ = (None,) * len(PutParams._fields)

class XferGetParams(Structure): # pylint: disable=too-few-public-methods, too-many-instance-attributes
    """Phobos GET parameters of the XferDescriptor."""
    _fields_ = [
        ("_node_name", c_char_p),
    ]

    def __init__(self):
        super().__init__()
        self._node_name = None

    @property
    def node_name(self):
        """Wrapper to get node_name"""
        return self._node_name.decode('utf-8') if self._node_name else None

class GetParams(namedtuple('GetParams', '')):
    """
    Transition data structure for get parameters between
    the CLI and the XFer data structure.
    """
GetParams.__new__.__defaults__ = (None,) * len(GetParams._fields)

class XferOpParams(Union): # pylint: disable=too-few-public-methods
    """Phobos operation parameters of the XferDescriptor."""
    _fields_ = [
        ("put", XferPutParams),
        ("get", XferGetParams),
    ]

class XferDescriptor(Structure): # pylint: disable=too-many-instance-attributes
    """phobos struct xfer_descriptor."""
    _fields_ = [
        ("_xd_objid", c_char_p),
        ("_xd_objuuid", c_char_p),
        ("xd_version", c_int),
        ("xd_op", c_int),
        ("xd_fd", c_int),
        ("xd_attrs", PhoAttrs),
        ("xd_params", XferOpParams),
        ("xd_flags", c_int),
        ("xd_rc", c_int),
    ]

    def __init__(self):
        super().__init__()
        self.xd_fd = -1
        self.xd_op = -1
        self.xd_flags = 0
        self.xd_rc = 0
        self.xd_version = -1

    @property
    def xd_objid(self):
        """Wrapper to get xd_objid"""
        return self._xd_objid.decode('utf-8') if self._xd_objid else None

    @xd_objid.setter
    def xd_objid(self, val):
        """Wrapper to set xd_objid"""
        # pylint: disable=attribute-defined-outside-init
        self._xd_objid = val.encode('utf-8') if val else None

    @property
    def xd_objuuid(self):
        """Wrapper to get xd_objuuid"""
        return self._xd_objuuid.decode('utf-8') if self._xd_objuuid else None

    @xd_objuuid.setter
    def xd_objuuid(self, val):
        """Wrapper to set xd_objuuid"""
        # pylint: disable=attribute-defined-outside-init
        self._xd_objuuid = val.encode('utf-8') if val else None

    def creat_flags(self):
        """
        Select the open flags depending on the operation made on the object
        when the file is created.
        Return the selected flags.
        """
        if self.xd_flags & PHO_XFER_OBJ_REPLACE:
            return os.O_CREAT | os.O_WRONLY | os.O_TRUNC

        return os.O_CREAT | os.O_WRONLY | os.O_EXCL

    def open_file(self, path):
        """
        Retrieve the xd_fd field of the xfer, opening path with NOATIME if
        the xfer has not been previously opened.
        Return the fd or raise OSError if the open failed or ValueError if
        the given path is not correct.
        """
        # in case of getmd, the file is not opened, return without exception
        if self.xd_op == PHO_XFER_OP_GETMD:
            self.xd_fd = -1
            return

        if not path:
            raise ValueError("path must be a non empty string")

        if self.xd_op == PHO_XFER_OP_GET:
            self.xd_fd = os.open(path, self.creat_flags(), 0o666)
        else:
            try:
                self.xd_fd = os.open(path, os.O_RDONLY | os.O_NOATIME)
            except OSError as exc:
                # not allowed to open with NOATIME arg, try without
                if exc.errno != errno.EPERM:
                    raise exc
                self.xd_fd = os.open(path, os.O_RDONLY)

            self.xd_params.put.size = os.fstat(self.xd_fd).st_size

    def init_from_descriptor(self, desc):
        """
        xfer_descriptor initialization by using python-list descriptor.
        It opens the file descriptor of the given path. The python-list
        contains the tuple (id, path, attrs, flags, put or get parameters, op)
        describing the opened file.
        """
        self.xd_op = desc[5]
        if self.xd_op == PHO_XFER_OP_PUT:
            self.xd_params.put = XferPutParams(desc[4]) if \
                                 desc[4] is not None else \
                                 XferPutParams(PutParams())
        elif self.xd_op == PHO_XFER_OP_GET:
            self.xd_objuuid = desc[4][0]
            self.xd_version = desc[4][1]
            self.xd_params.get = XferGetParams()

        self.xd_objid = desc[0]
        self.xd_flags = desc[3]
        self.xd_rc = 0

        if desc[2]:
            for k, v in desc[2].items():
                LIBPHOBOS.pho_attr_set(byref(self.xd_attrs),
                                       str(k).encode('utf8'),
                                       str(v).encode('utf8'))

        self.open_file(desc[1])

XFER_COMPLETION_CB_TYPE = CFUNCTYPE(None, c_void_p, POINTER(XferDescriptor),
                                    c_int)

def compl_cb_convert(compl_cb):
    """
    Internal conversion method to turn a python callable into a C
    completion callback as expected by phobos_{get,put} functions.
    """
    if not callable(compl_cb):
        raise TypeError("Completion handler must be callable")

    return XFER_COMPLETION_CB_TYPE(compl_cb)

class Store:
    """Store handler"""
    def __init__(self, *args, **kwargs):
        """Initialize new instance."""
        super(Store, self).__init__(*args, **kwargs)
        # Keep references to CFUNCTYPES objects as long as they can be
        # called by the underlying C code, so that they do not get GC'd
        self.logger = logging.getLogger(__name__)
        self._cb = None

    @staticmethod
    def xfer_desc_convert(xfer_descriptors):
        """
        Internal conversion method to turn a python list into an array of
        struct xfer_descriptor as expected by phobos_{get,put} functions.
        xfer_descriptors is a list of (id, path, attrs, flags, tags, op).
        The element conversion is made by the xfer_descriptor initializer.
        """
        xfer_array_type = XferDescriptor * len(xfer_descriptors)
        xfr = xfer_array_type()
        for i, val in enumerate(xfer_descriptors):
            xfr[i].init_from_descriptor(val)

        return xfr

    @staticmethod
    def xfer_desc_release(xfer):
        """
        Release memory associated to xfer_descriptors in both phobos and cli.
        """
        for elt in xfer:
            if elt.xd_fd >= 0:
                os.close(elt.xd_fd)

            LIBPHOBOS.pho_xfer_desc_clean(byref(elt))

    def phobos_xfer(self, action_func, xfer_descriptors, compl_cb):
        """Wrapper for phobos_xfer API calls."""
        xfer = self.xfer_desc_convert(xfer_descriptors)
        n_xfer = len(xfer_descriptors)
        self._cb = compl_cb_convert(compl_cb)
        rc = action_func(xfer, n_xfer, self._cb, None)
        node_name = (xfer[0].xd_params.get.node_name
                     if xfer[0].xd_op == PHO_XFER_OP_GET
                     else None)
        self.xfer_desc_release(xfer)
        return rc, node_name

class XferClient: # pylint: disable=too-many-instance-attributes
    """Main class: issue data transfers with the object store."""
    def __init__(self, **kwargs):
        """Initialize a new instance."""
        super(XferClient, self).__init__(**kwargs)
        self.logger = logging.getLogger(__name__)
        self._store = Store()
        self.getmd_session = []
        self.get_session = []
        self.put_session = []
        self._getmd_cb = None
        self._get_cb = None
        self._put_cb = None

    def noop_compl_cb(self, *args, **kwargs):
        """Default, empty transfer completion handler."""

    def getmd_register(self, oid, data_path, attrs=None):
        """Enqueue a GETMD transfer."""
        self.getmd_session.append((oid, data_path, attrs, 0, None,
                                   PHO_XFER_OP_GETMD))

    def get_register(self, oid, data_path, get_args, best_host, attrs=None):
        # pylint: disable=too-many-arguments
        """Enqueue a GET transfer."""
        flags = PHO_XFER_OBJ_BEST_HOST if best_host else 0
        self.get_session.append((oid, data_path, attrs, flags, get_args,
                                 PHO_XFER_OP_GET))

    def put_register(self, oid, data_path, attrs=None,
                     put_params=PutParams()):
        """Enqueue a PUT transfert."""
        self.put_session.append((oid, data_path, attrs, 0, put_params,
                                 PHO_XFER_OP_PUT))

    def clear(self):
        """Release resources associated to the current queues."""
        self.getmd_session = []
        self._getmd_cb = None
        self.get_session = []
        self._get_cb = None
        self.put_session = []
        self._put_cb = None

    def run(self, compl_cb=None):
        """Execute all registered transfer orders."""
        if compl_cb is None:
            compl_cb = self.noop_compl_cb

        if self.getmd_session:
            rc, _ = self._store.phobos_xfer(LIBPHOBOS.phobos_getmd,
                                            self.getmd_session, compl_cb)
            if rc:
                full_oids = ", ".join([str(x[0]) for x in self.getmd_session])
                raise IOError(rc, "Cannot GETMD for objid(s) '%s'" % full_oids)

        if self.get_session:
            rc, node_name = self._store.phobos_xfer(LIBPHOBOS.phobos_get,
                                                    self.get_session,
                                                    compl_cb)

            if node_name is not None:
                print("Current host is not the best to get this object, try " +
                      "on this other node, '" + self.get_session[0][0] +
                      "' : '" + node_name + "'")

            if rc:
                for desc in self.get_session:
                    os.remove(desc[1])

                full_oids = ", ".join([str(x[0]) for x in self.get_session])
                full_paths = ", ".join([str(x[1]) for x in self.get_session])
                raise IOError(rc, "Cannot GET objid(s) '%s' to '%s'" %
                              (full_oids, full_paths))

        if self.put_session:
            rc, _ = self._store.phobos_xfer(LIBPHOBOS.phobos_put,
                                            self.put_session, compl_cb)
            if rc:
                full_oids = ", ".join([str(x[0]) for x in self.put_session])
                full_paths = ", ".join([str(x[1]) for x in self.put_session])
                raise IOError(rc, "Cannot PUT '%s' to objid(s) '%s'" %
                              (full_paths, full_oids))

        self.clear()

class UtilClient:
    """Secondary class: issue user commands without data transfers."""
    def __init__(self, **kwargs):
        """Initialize a new instance."""
        super(UtilClient, self).__init__(**kwargs)
        self.logger = logging.getLogger(__name__)

    @staticmethod
    def object_delete(oids, hard_delete):
        """Delete objects."""
        n_xfers = c_int(len(oids))
        xfer_array_type = XferDescriptor * len(oids)
        xfers = xfer_array_type()

        for i, oid in enumerate(oids):
            xfers[i].xd_objid = oid
            xfers[i].xd_flags = PHO_XFER_OBJ_HARD_DEL if hard_delete else 0

        rc = LIBPHOBOS.phobos_delete(xfers, n_xfers)
        if rc:
            raise EnvironmentError(rc, "Failed to delete objects '%s'" % oids)

    @staticmethod
    def object_undelete(oids, uuids):
        """Undelete objects."""
        n_xfers = c_int(len(oids) + len(uuids))
        xfer_array_type = XferDescriptor * (len(oids) + len(uuids))
        xfers = xfer_array_type()

        for i, oid in enumerate(oids):
            xfers[i].xd_objid = oid

        for i, uuid in enumerate(uuids):
            xfers[i + len(oids)].xd_objuuid = uuid

        rc = LIBPHOBOS.phobos_undelete(xfers, n_xfers)
        if rc:
            raise EnvironmentError(rc, "Failed to undelete objects by %s '%s'" %
                                   ("oids" if oids else "uuids",
                                    oids if oids else uuids))

    @staticmethod
    def object_list(res, is_pattern, metadata, deprecated, status_number,
                    **kwargs): # pylint: disable=too-many-arguments,too-many-locals
        """List objects."""
        n_objs = c_int(0)
        obj_type = ObjectInfo if not deprecated else DeprecatedObjectInfo
        objs = POINTER(obj_type)()

        n_status_number = c_int(status_number)

        enc_res = [elt.encode('utf-8') for elt in res]
        c_res_strlist = c_char_p * len(enc_res)

        enc_metadata = [md.encode('utf-8') for md in metadata]
        c_md_strlist = c_char_p * len(metadata)

        sort, kwargs = dss_sort('object', **kwargs)
        sref = byref(sort) if sort else None

        rc = LIBPHOBOS.phobos_store_object_list(c_res_strlist(*enc_res),
                                                len(enc_res),
                                                is_pattern,
                                                c_md_strlist(*enc_metadata),
                                                len(metadata),
                                                deprecated,
                                                n_status_number,
                                                byref(objs),
                                                byref(n_objs),
                                                sref)

        if rc:
            raise EnvironmentError(rc, "Failed to list %s" %
                                   ("object(s) '%s'" % res
                                    if res else "all objects"))

        objs = (obj_type * n_objs.value).from_address(
            cast(objs, c_void_p).value)

        return objs

    @staticmethod
    def list_free(objs, n_objs):
        """Free a previously obtained object list."""
        LIBPHOBOS.phobos_store_object_list_free(objs, n_objs)

    @staticmethod
    def object_rename(old_oid, uuid, new_oid):
        """Rename an object"""
        rc = LIBPHOBOS.phobos_rename(
            old_oid.encode('utf-8') if old_oid else None,
            uuid.encode('utf-8') if uuid else None,
            new_oid.encode('utf-8'))
        if rc:
            raise EnvironmentError(rc, "Failed to rename object of %s" %
                                   ("%s = '%s' to '%s'" %
                                    ("oid" if old_oid else "uuid",
                                     old_oid if old_oid else uuid, new_oid)))

    @staticmethod
    def object_locate(oid, uuid, version, focus_host):
        """Locate an object"""
        hostname = c_char_p(None)
        nb_new_lock = c_int(0)
        rc = LIBPHOBOS.phobos_locate(
            oid.encode('utf-8'),
            uuid.encode('utf-8') if uuid else None,
            version,
            focus_host.encode('utf-8') if focus_host else None,
            byref(hostname), byref(nb_new_lock))
        if rc:
            raise EnvironmentError(rc, "Failed to locate object by %s '%s'" %
                                   ("oid" if oid else "uuid",
                                    oid if oid else uuid))

        return (hostname.value.decode('utf-8') if hostname.value else "",
                nb_new_lock)
