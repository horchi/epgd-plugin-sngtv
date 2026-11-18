/*
 * jpegscale.h
 *
 * Scale a jpeg (in memory) down to fit into a box - libjpeg scales while
 * decoding in steps of 1/8, so this is cheap.
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

#include <jpeglib.h>

// returns true if 'out' holds a scaled picture,
//   false if no scaling is needed (picture already small enough) or on error ('error' is set)

inline bool scaleJpeg(const unsigned char* in, unsigned long inSize, int maxW, int maxH, int quality,
                      std::vector<unsigned char>& out, unsigned& outW, unsigned& outH, std::string& error)
{
   struct jpeg_decompress_struct dinfo {};
   struct jpeg_compress_struct cinfo {};
   struct jpeg_error_mgr jerr {};
   unsigned char* buf {};
   unsigned long bufSize {0};
   bool compressCreated {false};

   error.clear();
   out.clear();

   if (!inSize || (!maxW && !maxH))
      return false;

   dinfo.err = jpeg_std_error(&jerr);
   jerr.error_exit = [](j_common_ptr info) { throw (int)info->err->msg_code; };
   jerr.output_message = [](j_common_ptr) {};

   try
   {
      jpeg_create_decompress(&dinfo);
      jpeg_mem_src(&dinfo, in, inSize);
      jpeg_read_header(&dinfo, TRUE);

      // largest scale n/8 which fits into the box

      double fW = maxW ? (double)maxW / dinfo.image_width : 1.0;
      double fH = maxH ? (double)maxH / dinfo.image_height : 1.0;
      int num = std::max(1, std::min(8, (int)(std::min(fW, fH) * 8 + 0.0001)));

      if (num >= 8)
      {
         jpeg_destroy_decompress(&dinfo);
         return false;
      }

      dinfo.scale_num = num;
      dinfo.scale_denom = 8;
      dinfo.out_color_space = JCS_RGB;
      jpeg_start_decompress(&dinfo);

      cinfo.err = dinfo.err;
      jpeg_create_compress(&cinfo);
      compressCreated = true;
      jpeg_mem_dest(&cinfo, &buf, &bufSize);
      cinfo.image_width = dinfo.output_width;
      cinfo.image_height = dinfo.output_height;
      cinfo.input_components = 3;
      cinfo.in_color_space = JCS_RGB;
      jpeg_set_defaults(&cinfo);
      jpeg_set_quality(&cinfo, quality, TRUE);
      jpeg_start_compress(&cinfo, TRUE);

      std::vector<unsigned char> row(dinfo.output_width * 3);
      JSAMPROW rowPtr = row.data();

      while (dinfo.output_scanline < dinfo.output_height)
      {
         jpeg_read_scanlines(&dinfo, &rowPtr, 1);
         jpeg_write_scanlines(&cinfo, &rowPtr, 1);
      }

      jpeg_finish_compress(&cinfo);
      jpeg_finish_decompress(&dinfo);

      outW = cinfo.image_width;
      outH = cinfo.image_height;
      out.assign(buf, buf + bufSize);

      jpeg_destroy_compress(&cinfo);
      jpeg_destroy_decompress(&dinfo);
      free(buf);
   }
   catch (int code)
   {
      error = "libjpeg error " + std::to_string(code);

      if (compressCreated)
         jpeg_destroy_compress(&cinfo);

      jpeg_destroy_decompress(&dinfo);
      free(buf);
      return false;
   }

   return true;
}
