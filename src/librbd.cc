// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2.1, as published by the Free Software
 * Foundation.	See file COPYING.
 *
 */

#define __STDC_FORMAT_MACROS
#include "config.h"

#include "common/common_init.h"
#include "include/librbd.hpp"
#include "include/byteorder.h"

#include "include/intarith.h"

#include <errno.h>
#include <inttypes.h>
#include <iostream>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>

#include <sys/ioctl.h>

#include "include/rbd_types.h"

#include <linux/fs.h>

#include "include/fiemap.h"

#define DOUT_SUBSYS rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd: "

int librbd::RBD::initialize(int argc, const char *argv[])
{
 vector<const char*> args;

  if (argc && argv) {
    argv_to_vec(argc, argv, args);
    env_to_vec(args);
  }

  common_set_defaults(false);
  common_init(args, "rbd", true);

  if (rados.initialize(argc, argv) < 0) {
    return -1;
  }
  return 0;
}

void librbd::RBD::shutdown()
{
  rados.shutdown();
}

void librbd::RBD::version(int *major, int *minor, int *extra)
{
  librbd_version(major, minor, extra);
}

int librbd::RBD::open_pools(const char *poolname, pools_t& pp)
{
  librados::pool_t pool, md_pool;

  int r = rados.open_pool(poolname, &md_pool);
  if (r < 0) {
    cerr << "error opening pool " << poolname << " (err=" << r << ")" << std::endl;
    return -1;
  }
  pp.md = md_pool;

  r = rados.open_pool(poolname, &pool);
  if (r < 0) {
    cerr << "error opening pool " << poolname << " (err=" << r << ")" << std::endl;
    close_pools(pp);
    return -1;
  }
  pp.data = pool;

  return 0; 
}

int librbd::RBD::create_image(const char *pool, const char *name, size_t size)
{
  pools_t pp = { 0, 0, 0 };
  if (open_pools(pool, pp) < 0)
    return -1;
  string md_oid = name;
  md_oid += RBD_SUFFIX;
  int order = 0;
  int r = do_create(pp.md, md_oid, name, size, &order);
  close_pools(pp);
  return r;
}

int librbd::RBD::remove_image(const char *pool, const char *name)
{
  pools_t pp = { 0, 0, 0 };
  if (open_pools(pool, pp) < 0)
    return -1;
  string md_oid = name;
  md_oid += RBD_SUFFIX;
  int r = do_delete(pp, md_oid, name);
  close_pools(pp);
  return r;
}

int librbd::RBD::resize_image(const char *pool, const char *name, size_t size)
{
  pools_t pp = { 0, 0, 0 };
  if (open_pools(pool, pp) < 0)
    return -1;
  string md_oid = name;
  md_oid += RBD_SUFFIX;
  int r = do_resize(pp, md_oid, name, size);
  close_pools(pp);
  return r;
}

int librbd::RBD::stat_image(const char *pool, const char *name, image_info_t& info)
{
  pools_t pp = { 0, 0, 0 };
  if (open_pools(pool, pp) < 0)
    return -1;
  string md_oid = name;
  md_oid += RBD_SUFFIX;
  int r = do_info(pp, md_oid, info);
  close_pools(pp);
  return r;
}

int librbd::RBD::list_images(const char *pool, std::vector<string>& names)
{
  pools_t pp = { 0, 0, 0 };
  if (open_pools(pool, pp) < 0)
    return -1;
  int r = do_list(pp, pool, names);
  close_pools(pp);
  return r;
}

int librbd::RBD::create_snap(const char *pool, const char *image_name, const char *snapname)
{
  pools_t pp = { 0, 0, 0 };
  librados::snap_t snapid = 0;
  vector<librados::snap_t> snaps;
  ::SnapContext snapc;
  string md_oid = image_name;
  md_oid += RBD_SUFFIX;

  if (open_pools(pool, pp) < 0)
    return -1;
  int r = do_get_snapc(pp, md_oid, snapname, snapc, snaps, snapid);
  if (r != -ENOENT && r < 0)
    return r;

  r = rados.set_snap_context(pp.data, snapc.seq, snaps);
  if (r < 0)
    return r;

  rados.set_snap(pp.data, snapid);
  r = do_add_snap(pp, md_oid, snapname);
  close_pools(pp);
  return r;
}

int librbd::RBD::remove_snap(const char *pool, const char *image_name, const char *snapname)
{
  pools_t pp = { 0, 0, 0 };
  librados::snap_t snapid = 0;
  vector<librados::snap_t> snaps;
  ::SnapContext snapc;
  string md_oid = image_name;
  md_oid += RBD_SUFFIX;

  if (open_pools(pool, pp) < 0)
    return -1;
  int r = do_get_snapc(pp, md_oid, snapname, snapc, snaps, snapid);
  if (r < 0)
    return r;

  r = rados.set_snap_context(pp.data, snapc.seq, snaps);
  if (r < 0)
    return r;

  rados.set_snap(pp.data, snapid);
  r = do_remove_snap(pp, md_oid, snapname, snapid);
  close_pools(pp);
  return r;
}

int librbd::RBD::rollback_snap(const char *pool, const char *image_name, const char *snapname)
{
  pools_t pp = { 0, 0, 0 };
  librados::snap_t snapid = 0;
  vector<librados::snap_t> snaps;
  ::SnapContext snapc;
  string md_oid = image_name;
  md_oid += RBD_SUFFIX;

  if (open_pools(pool, pp) < 0)
    return -1;
  int r = do_get_snapc(pp, md_oid, snapname, snapc, snaps, snapid);
  if (r < 0)
    return r;

  r = rados.set_snap_context(pp.data, snapc.seq, snaps);
  if (r < 0)
    return r;

  rados.set_snap(pp.data, snapid);
  r = do_rollback_snap(pp, md_oid, snapc, snapid);
  close_pools(pp);
  return r;
}

int librbd::RBD::list_snaps(const char *pool, const char* image_name, std::vector<librbd::snap_info_t>& snaps)
{
  pools_t pp = { 0, 0, 0 };
  string md_oid = image_name;
  md_oid += RBD_SUFFIX;
  if (open_pools(pool, pp) < 0)
    return -1;
  int r = do_list_snaps(pp, md_oid, snaps);
  close_pools(pp);
  return r;
}

void librbd::RBD::init_rbd_header(struct rbd_obj_header_ondisk& ondisk,
				  size_t size, int *order, uint64_t bid)
{
  uint32_t hi = bid >> 32;
  uint32_t lo = bid & 0xFFFFFFFF;
  memset(&ondisk, 0, sizeof(ondisk));

  memcpy(&ondisk.text, RBD_HEADER_TEXT, sizeof(RBD_HEADER_TEXT));
  memcpy(&ondisk.signature, RBD_HEADER_SIGNATURE, sizeof(RBD_HEADER_SIGNATURE));
  memcpy(&ondisk.version, RBD_HEADER_VERSION, sizeof(RBD_HEADER_VERSION));

  snprintf(ondisk.block_name, sizeof(ondisk.block_name), "rb.%x.%x", hi, lo);

  if (!*order)
    *order = RBD_DEFAULT_OBJ_ORDER;

  ondisk.image_size = size;
  ondisk.options.order = *order;
  ondisk.options.crypt_type = RBD_CRYPT_NONE;
  ondisk.options.comp_type = RBD_COMP_NONE;
  ondisk.snap_seq = 0;
  ondisk.snap_count = 0;
  ondisk.reserved = 0;
  ondisk.snap_names_len = 0;
}

void librbd::RBD::image_info(rbd_obj_header_ondisk& header, librbd::image_info_t& info)
{
  int obj_order = header.options.order;
  info.size = header.image_size;
  info.obj_size = 1 << obj_order;
  info.num_objs = header.image_size >> obj_order;
  info.order = obj_order;
}

string librbd::RBD::get_block_oid(rbd_obj_header_ondisk *header, uint64_t num)
{
  char o[RBD_MAX_SEG_NAME_SIZE];
  snprintf(o, RBD_MAX_SEG_NAME_SIZE,
       "%s.%012" PRIx64, header->block_name, num);
  return o;
}

uint64_t librbd::RBD::get_max_block(rbd_obj_header_ondisk *header)
{
  uint64_t size = header->image_size;
  int obj_order = header->options.order;
  uint64_t block_size = 1 << obj_order;
  uint64_t numseg = (size + block_size - 1) >> obj_order;

  return numseg;
}

uint64_t librbd::RBD::get_block_size(rbd_obj_header_ondisk *header)
{
  return 1 << header->options.order;
}

uint64_t librbd::RBD::get_block_num(rbd_obj_header_ondisk *header, uint64_t ofs)
{
  int obj_order = header->options.order;
  uint64_t num = ofs >> obj_order;

  return num;
}

int librbd::RBD::init_rbd_info(struct rbd_info *info)
{
  memset(info, 0, sizeof(*info));
  return 0;
}

void librbd::RBD::trim_image(pools_t& pp, const char *imgname, rbd_obj_header_ondisk *header, uint64_t newsize)
{
  uint64_t numseg = get_max_block(header);
  uint64_t start = get_block_num(header, newsize);

  cout << "trimming image data from " << numseg << " to " << start << " objects..." << std::endl;
  for (uint64_t i=start; i<numseg; i++) {
    string oid = get_block_oid(header, i);
    rados.remove(pp.data, oid);
    if ((i & 127) == 0) {
      cout << "\r\t" << i << "/" << numseg;
      cout.flush();
    }
  }
}

int librbd::RBD::read_rbd_info(pools_t& pp, string& info_oid, struct rbd_info *info)
{
  int r;
  bufferlist bl;

  r = rados.read(pp.md, info_oid, 0, bl, sizeof(*info));
  if (r < 0)
    return r;
  if (r == 0) {
    return init_rbd_info(info);
  }

  if (r < (int)sizeof(*info))
    return -EIO;

  memcpy(info, bl.c_str(), r);
  return 0;
}

int librbd::RBD::touch_rbd_info(librados::pool_t pool, string& info_oid)
{
  bufferlist bl;
  int r = rados.write(pool, info_oid, 0, bl, 0);
  if (r < 0)
    return r;
  return 0;
}

int librbd::RBD::rbd_assign_bid(librados::pool_t pool, string& info_oid, uint64_t *id)
{
  bufferlist bl, out;

  *id = 0;

  int r = touch_rbd_info(pool, info_oid);
  if (r < 0)
    return r;

  r = rados.exec(pool, info_oid, "rbd", "assign_bid", bl, out);
  if (r < 0)
    return r;

  bufferlist::iterator iter = out.begin();
  ::decode(*id, iter);

  return 0;
}


int librbd::RBD::read_header_bl(librados::pool_t pool, string& md_oid, bufferlist& header, uint64_t *ver)
{
  int r;
#define READ_SIZE 4096
  do {
    bufferlist bl;
    r = rados.read(pool, md_oid, 0, bl, READ_SIZE);
    if (r < 0)
      return r;
    header.claim_append(bl);
   } while (r == READ_SIZE);

  if (ver)
    *ver = rados.get_last_version(pool);

  return 0;
}

int librbd::RBD::notify_change(librados::pool_t pool, string& oid, uint64_t *pver)
{
  uint64_t ver;
  if (pver)
    ver = *pver;
  else
    ver = rados.get_last_version(pool);
  rados.notify(pool, oid, ver);
  return 0;
}

int librbd::RBD::read_header(librados::pool_t pool, string& md_oid, struct rbd_obj_header_ondisk *header, uint64_t *ver)
{
  bufferlist header_bl;
  int r = read_header_bl(pool, md_oid, header_bl, ver);
  if (r < 0)
    return r;
  if (header_bl.length() < (int)sizeof(*header))
    return -EIO;
  memcpy(header, header_bl.c_str(), sizeof(*header));

  return 0;
}

int librbd::RBD::write_header(pools_t& pp, string& md_oid, bufferlist& header)
{
  bufferlist bl;
  int r = rados.write(pp.md, md_oid, 0, header, header.length());

  notify_change(pp.md, md_oid, NULL);

  return r;
}

int librbd::RBD::tmap_set(pools_t& pp, string& imgname)
{
  bufferlist cmdbl, emptybl;
  __u8 c = CEPH_OSD_TMAP_SET;
  ::encode(c, cmdbl);
  ::encode(imgname, cmdbl);
  ::encode(emptybl, cmdbl);
  return rados.tmap_update(pp.md, RBD_DIRECTORY, cmdbl);
}

int librbd::RBD::tmap_rm(pools_t& pp, string& imgname)
{
  bufferlist cmdbl;
  __u8 c = CEPH_OSD_TMAP_RM;
  ::encode(c, cmdbl);
  ::encode(imgname, cmdbl);
  return rados.tmap_update(pp.md, RBD_DIRECTORY, cmdbl);
}

int librbd::RBD::rollback_image(pools_t& pp, struct rbd_obj_header_ondisk *header,
				::SnapContext& snapc, uint64_t snapid)
{
  uint64_t numseg = get_max_block(header);

  for (uint64_t i = 0; i < numseg; i++) {
    int r;
    string oid = get_block_oid(header, i);
    librados::SnapContext sn;
    sn.seq = snapc.seq;
    sn.snaps.clear();
    vector<snapid_t>::iterator iter = snapc.snaps.begin();
    for (; iter != snapc.snaps.end(); ++iter) {
      sn.snaps.push_back(*iter);
    }
    r = rados.selfmanaged_snap_rollback_object(pp.data, oid, sn, snapid);
    if (r < 0 && r != -ENOENT)
      return r;
  }
  return 0;
}

int librbd::RBD::do_list(pools_t& pp, const char *poolname, std::vector<string>& names)
{
  bufferlist bl;
  int r = rados.read(pp.md, RBD_DIRECTORY, 0, bl, 0);
  if (r < 0)
    return r;

  bufferlist::iterator p = bl.begin();
  bufferlist header;
  map<string,bufferlist> m;
  ::decode(header, p);
  ::decode(m, p);
  for (map<string,bufferlist>::iterator q = m.begin(); q != m.end(); q++)
    names.push_back(q->first);
  return 0;
}

int librbd::RBD::do_create(librados::pool_t pool, string& md_oid, const char *imgname,
			   uint64_t size, int *order)
{

  // make sure it doesn't already exist
  int r = rados.stat(pool, md_oid, NULL, NULL);
  if (r == 0) {
    cerr << "rbd image header " << md_oid << " already exists" << std::endl;
    return -EEXIST;
  }

  uint64_t bid;
  string dir_info = RBD_INFO;
  r = rbd_assign_bid(pool, dir_info, &bid);
  if (r < 0) {
    cerr << "failed to assign a block name for image" << std::endl;
    return r;
  }

  struct rbd_obj_header_ondisk header;
  init_rbd_header(header, size, order, bid);

  bufferlist bl;
  bl.append((const char *)&header, sizeof(header));

  cout << "adding rbd image to directory..." << std::endl;
  bufferlist cmdbl, emptybl;
  __u8 c = CEPH_OSD_TMAP_SET;
  ::encode(c, cmdbl);
  ::encode(imgname, cmdbl);
  ::encode(emptybl, cmdbl);
  r = rados.tmap_update(pool, RBD_DIRECTORY, cmdbl);
  if (r < 0) {
    cerr << "error adding img to directory: " << strerror(-r)<< std::endl;
    return r;
  }

  cout << "creating rbd image..." << std::endl;
  r = rados.write(pool, md_oid, 0, bl, bl.length());
  if (r < 0) {
    cerr << "error writing header: " << strerror(-r) << std::endl;
    return r;
  }

  cout << "done." << std::endl;
  return 0;
}

int librbd::RBD::do_rename(pools_t& pp, string& md_oid, const char *imgname, const char *dstname)
{
  string dst_md_oid = dstname;
  dst_md_oid += RBD_SUFFIX;
  string dstname_str = dstname;
  string imgname_str = imgname;
  uint64_t ver;
  bufferlist header;
  int r = read_header_bl(pp.md, md_oid, header, &ver);
  if (r < 0) {
    cerr << "error reading header: " << md_oid << ": " << strerror(-r) << std::endl;
    return r;
  }
  r = rados.stat(pp.md, dst_md_oid, NULL, NULL);
  if (r == 0) {
    cerr << "rbd image header " << dst_md_oid << " already exists" << std::endl;
    return -EEXIST;
  }
  r = write_header(pp, dst_md_oid, header);
  if (r < 0) {
    cerr << "error writing header: " << dst_md_oid << ": " << strerror(-r) << std::endl;
    return r;
  }
  r = tmap_set(pp, dstname_str);
  if (r < 0) {
    rados.remove(pp.md, dst_md_oid);
    cerr << "can't add " << dst_md_oid << " to directory" << std::endl;
    return r;
  }
  r = tmap_rm(pp, imgname_str);
  if (r < 0)
    cerr << "warning: couldn't remove old entry from directory (" << imgname_str << ")" << std::endl;

  r = rados.remove(pp.md, md_oid);
  if (r < 0)
    cerr << "warning: couldn't remove old metadata" << std::endl;

  return 0;
}


int librbd::RBD::do_info(pools_t& pp, string& md_oid, librbd::image_info_t& info)
{
  struct rbd_obj_header_ondisk header;
  int r = read_header(pp.md, md_oid, &header, NULL);
  if (r < 0)
    return r;

  image_info(header, info);
  return 0;
}

int librbd::RBD::do_delete(pools_t& pp, string& md_oid, const char *imgname)
{
  struct rbd_obj_header_ondisk header;
  int r = read_header(pp.md, md_oid, &header, NULL);
  if (r >= 0) {
    trim_image(pp, imgname, &header, 0);
    cout << "\rremoving header..." << std::endl;
    rados.remove(pp.md, md_oid);
  }

  cout << "removing rbd image to directory..." << std::endl;
  bufferlist cmdbl;
  __u8 c = CEPH_OSD_TMAP_RM;
  ::encode(c, cmdbl);
  ::encode(imgname, cmdbl);
  r = rados.tmap_update(pp.md, RBD_DIRECTORY, cmdbl);
  if (r < 0) {
    cerr << "error removing img from directory: " << strerror(-r)<< std::endl;
    return r;
  }

  cout << "done." << std::endl;
  return 0;
}

int librbd::RBD::do_resize(pools_t& pp, string& md_oid, const char *imgname, uint64_t size)
{
  struct rbd_obj_header_ondisk header;
  uint64_t ver;
  int r = read_header(pp.md, md_oid, &header, &ver);
  if (r < 0)
    return r;

  // trim
  if (size == header.image_size) {
    cout << "no change in size (" << size << " -> " << header.image_size << ")" << std::endl;
    return 0;
  }

  if (size > header.image_size) {
    cout << "expanding image " << size << " -> " << header.image_size << " objects" << std::endl;
    header.image_size = size;
  } else {
    cout << "shrinking image " << size << " -> " << header.image_size << " objects" << std::endl;
    trim_image(pp, imgname, &header, size);
    header.image_size = size;
  }

  // rewrite header
  bufferlist bl;
  bl.append((const char *)&header, sizeof(header));
  rados.set_assert_version(pp.md, ver);
  r = rados.write(pp.md, md_oid, 0, bl, bl.length());
  if (r == -ERANGE)
    cerr << "operation might have conflicted with another client!" << std::endl;
  if (r < 0) {
    cerr << "error writing header: " << strerror(-r) << std::endl;
    return r;
  } else {
    notify_change(pp.md, md_oid, NULL);
  }

  cout << "done." << std::endl;
  return 0;
}

int librbd::RBD::do_list_snaps(pools_t& pp, string& md_oid, std::vector<librbd::snap_info_t>& snaps)
{
  bufferlist bl, bl2;

  int r = rados.exec(pp.md, md_oid, "rbd", "snap_list", bl, bl2);
  if (r < 0) {
    return r;
  }

  uint32_t num_snaps;
  uint64_t snap_seq;
  bufferlist::iterator iter = bl2.begin();
  ::decode(snap_seq, iter);
  ::decode(num_snaps, iter);
  for (uint32_t i=0; i < num_snaps; i++) {
    uint64_t id, image_size;
    string s;
    librbd::snap_info_t info;
    ::decode(id, iter);
    ::decode(image_size, iter);
    ::decode(s, iter);
    info.name = s;
    info.id = id;
    info.size = image_size;
    snaps.push_back(info);
  }
  return 0;
}

int librbd::RBD::do_add_snap(pools_t& pp, string& md_oid, const char *snapname)
{
  bufferlist bl, bl2;
  uint64_t snap_id;

  int r = rados.selfmanaged_snap_create(pp.md, &snap_id);
  if (r < 0) {
    cerr << "failed to create snap id: " << strerror(-r) << std::endl;
    return r;
  }

  ::encode(snapname, bl);
  ::encode(snap_id, bl);

  r = rados.exec(pp.md, md_oid, "rbd", "snap_add", bl, bl2);
  if (r < 0) {
    cerr << "rbd.snap_add execution failed failed: " << strerror(-r) << std::endl;
    return r;
  }
  notify_change(pp.md, md_oid, NULL);

  return 0;
}

int librbd::RBD::do_rm_snap(pools_t& pp, string& md_oid, const char *snapname)
{
  bufferlist bl, bl2;

  ::encode(snapname, bl);

  int r = rados.exec(pp.md, md_oid, "rbd", "snap_remove", bl, bl2);
  if (r < 0) {
    cerr << "rbd.snap_remove execution failed failed: " << strerror(-r) << std::endl;
    return r;
  }

  return 0;
}

int librbd::RBD::do_get_snapc(pools_t& pp, string& md_oid, const char *snapname,
			      ::SnapContext& snapc, vector<snap_t>& snaps, uint64_t& snapid)
{
  bufferlist bl, bl2;

  int r = rados.exec(pp.md, md_oid, "rbd", "snap_list", bl, bl2);
  if (r < 0) {
    cerr << "list_snaps failed: " << strerror(-r) << std::endl;
    return r;
  }

  snaps.clear();

  uint32_t num_snaps;
  bufferlist::iterator iter = bl2.begin();
  ::decode(snapc.seq, iter);
  ::decode(num_snaps, iter);
  snapid = 0;
  for (uint32_t i=0; i < num_snaps; i++) {
    uint64_t id, image_size;
    string s;
    ::decode(id, iter);
    ::decode(image_size, iter);
    ::decode(s, iter);
    if (s.compare(snapname) == 0)
      snapid = id;
    snapc.snaps.push_back(id);
    snaps.push_back(id);
  }

  if (!snapc.is_valid()) {
    cerr << "image snap context is invalid! can't rollback" << std::endl;
    return -EIO;
  }

  if (!snapid) {
    return -ENOENT;
  }

  return 0;
}

int librbd::RBD::do_rollback_snap(pools_t& pp, string& md_oid, ::SnapContext& snapc, uint64_t snapid)
{
  struct rbd_obj_header_ondisk header;
  int r = read_header(pp.md, md_oid, &header, NULL);
  if (r < 0) {
    cerr << "error reading header: " << md_oid << ": " << strerror(-r) << std::endl;
    return r;
  }
  r = rollback_image(pp, &header, snapc, snapid);
  if (r < 0)
    return r;

  return 0;
}

int librbd::RBD::do_remove_snap(pools_t& pp, string& md_oid, const char *snapname,
				uint64_t snapid)
{
  int r = do_rm_snap(pp, md_oid, snapname);
  r = rados.selfmanaged_snap_remove(pp.data, snapid);

  return r;
}

int librbd::RBD::do_copy(pools_t& pp, const char *imgname, const char *destname)
{
  struct rbd_obj_header_ondisk header, dest_header;
  int64_t ret;
  int r;
  string md_oid, dest_md_oid;

  md_oid = imgname;
  md_oid += RBD_SUFFIX;

  dest_md_oid = destname;
  dest_md_oid += RBD_SUFFIX;

  ret = read_header(pp.md, md_oid, &header, NULL);
  if (ret < 0)
    return ret;

  uint64_t numseg = get_max_block(&header);
  uint64_t block_size = get_block_size(&header);
  int order = header.options.order;

  r = do_create(pp.dest, dest_md_oid, destname, header.image_size, &order);
  if (r < 0) {
    cerr << "header creation failed" << std::endl;
    return r;
  }

  ret = read_header(pp.dest, dest_md_oid, &dest_header, NULL);
  if (ret < 0) {
    cerr << "failed to read newly created header" << std::endl;
    return ret;
  }

  for (uint64_t i = 0; i < numseg; i++) {
    bufferlist bl;
    string oid = get_block_oid(&header, i);
    string dest_oid = get_block_oid(&dest_header, i);
    map<off_t, size_t> m;
    map<off_t, size_t>::iterator iter;
    r = rados.sparse_read(pp.data, oid, 0, block_size, m, bl);
    if (r < 0 && r == -ENOENT)
      r = 0;
    if (r < 0)
      return r;


    for (iter = m.begin(); iter != m.end(); ++iter) {
      off_t extent_ofs = iter->first;
      size_t extent_len = iter->second;
      bufferlist wrbl;
      if (extent_ofs + extent_len > bl.length()) {
	cerr << "data error!" << std::endl;
	return -EIO;
      }
      bl.copy(extent_ofs, extent_len, wrbl);
      r = rados.write(pp.dest, dest_oid, extent_ofs, wrbl, extent_len);
      if (r < 0)
	goto done;
    }
  }
  r = 0;

done:
  return r;
}

void librbd::RBD::close_pools(pools_t& pp)
{
  if (pp.data)
    rados.close_pool(pp.data);
  if (pp.md)
    rados.close_pool(pp.md);
}

static librbd::RBD rbd;

extern "C" int rbd_initialize(int argc, const char **argv)
{
  return rbd.initialize(argc, argv);
}

extern "C" void rbd_shutdown()
{
  rbd.shutdown();
}

extern "C" void librbd_version(int *major, int *minor, int *extra)
{
  if (major)
    *major = LIBRBD_VER_MAJOR;
  if (minor)
    *minor = LIBRBD_VER_MINOR;
  if (extra)
    *extra = LIBRBD_VER_EXTRA;
}

/* images */
extern "C" int rbd_create_image(const char *pool, const char *name, size_t size)
{
  return rbd.create_image(pool, name, size);
}

extern "C" int rbd_remove_image(const char *pool, const char *name)
{
  return rbd.remove_image(pool, name);
}

extern "C" int rbd_resize_image(const char *pool, const char *name, size_t size)
{
  return rbd.resize_image(pool, name, size);
}

extern "C" int rbd_stat_image(const char *pool, const char *name, rbd_image_info_t *info)
{
  librbd::image_info_t cpp_info;
  int r = rbd.stat_image(pool, name, cpp_info);
  if (r < 0)
    return r;

  info->size = cpp_info.size;
  info->obj_size = cpp_info.obj_size;
  info->num_objs = cpp_info.num_objs;
  info->order = cpp_info.order;
  return 0;
}

extern "C" size_t rbd_list_images(const char *pool, char **names, size_t max_names)
{
  std::vector<string> cpp_names;
  int r = rbd.list_images(pool, cpp_names);
  if (r == -ENOENT)
    return 0;
  if (r < 0)
    return r;
  if (max_names < cpp_names.size())
    return -ERANGE;

  for (size_t i = 0; i < cpp_names.size(); i++) {
    names[i] = strdup(cpp_names[i].c_str());
    if (!names[i])
      return -ENOMEM;
  }
  return cpp_names.size();
}

/* snapshots */
extern "C" int rbd_create_snap(const char *pool, const char *image, const char *snapname)
{
  return rbd.create_snap(pool, image, snapname);
}

extern "C" int rbd_remove_snap(const char *pool, const char *image, const char *snapname)
{
  return rbd.remove_snap(pool, image, snapname);
}

extern "C" int rbd_rollback_snap(const char *pool, const char *image, const char *snapname)
{
  return rbd.rollback_snap(pool, image, snapname);
}

extern "C" size_t rbd_list_snaps(const char *pool, const char *image, rbd_snap_info_t *snaps, size_t max_snaps)
{
  std::vector<librbd::snap_info_t> cpp_snaps;
  int r = rbd.list_snaps(pool, image, cpp_snaps);
  if (r == -ENOENT)
    return 0;
  if (r < 0)
    return r;
  if (max_snaps < cpp_snaps.size())
    return -ERANGE;

  for (size_t i = 0; i < cpp_snaps.size(); i++) {
    snaps[i].id = cpp_snaps[i].id;
    snaps[i].size = cpp_snaps[i].size;
    snaps[i].name = strdup(cpp_snaps[i].name.c_str());
    if (!snaps[i].name)
      return -ENOMEM;
  }
  return cpp_snaps.size();
}
