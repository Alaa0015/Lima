//###########################################################################
// This file is part of LImA, a Library for Image Acquisition
//
// Copyright (C) : 2009-2017
// European Synchrotron Radiation Facility
// CS40220 38043 Grenoble Cedex 9 
// FRANCE
//
// Contact: lima@esrf.fr
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This software is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//###########################################################################

#include "lima/CtSaving_Compression.h"
#include "CtSaving_Edf.h"

using namespace lima;

const int _BufferHelper::BUFFER_HELPER_SIZE = 64 * 1024;

_BufferHelper::_BufferHelper()
{
  DEB_CONSTRUCTOR();

  _init(BUFFER_HELPER_SIZE);
}

_BufferHelper::_BufferHelper(int buffer_size)
{
  DEB_CONSTRUCTOR();
  DEB_PARAM() << DEB_VAR1(buffer_size);

  _init(buffer_size);
}

void _BufferHelper::_init(int buffer_size)
{
  DEB_MEMBER_FUNCT();
  DEB_PARAM() << DEB_VAR1(buffer_size);

  used_size = 0;
#ifdef __unix
  if(posix_memalign(&buffer,4*1024,buffer_size))
#else
  buffer = _aligned_malloc(buffer_size,4*1024);
  if(!buffer)
#endif
    THROW_CTL_ERROR(Error) << "Can't allocate buffer";
}

_BufferHelper::~_BufferHelper()
{
#ifdef __unix
  free(buffer);
#else
  _aligned_free(buffer);
#endif
}

#define DELETE_BUFFER_LIST(buflist) \
      for(ZBufferType::iterator i = buflist->begin(); \
   	i != buflist->end();++i) \
	  delete *i; \
      delete buflist;

#ifdef WITH_Z_COMPRESSION
FileZCompression::FileZCompression(CtSaving::SaveContainer &save_cnt,
			   int framesPerFile,const CtSaving::HeaderMap &header) :
  m_container(save_cnt),m_frame_per_file(framesPerFile)
{
  DEB_CONSTRUCTOR();
  
  m_compression_struct.next_in = NULL;
  m_compression_struct.avail_in = 0;
  m_compression_struct.total_in = 0;
  
  m_compression_struct.next_out = NULL;
  m_compression_struct.avail_out = 0;
  m_compression_struct.total_out = 0;
  
  m_compression_struct.zalloc = NULL;
  m_compression_struct.zfree = NULL;
  
  if(deflateInit2(&m_compression_struct, 8,
		  Z_DEFLATED,
		  31,
		  8,
		  Z_DEFAULT_STRATEGY) != Z_OK)
    THROW_CTL_ERROR(Error) << "Can't init compression struct";
};

FileZCompression::~FileZCompression()
{
  deflateEnd(&m_compression_struct);
}

void FileZCompression::process(Data &aData)
{
  ZBufferType *aBufferListPt = new ZBufferType();

  
  std::ostringstream buffer;
  try
    {
      SaveContainerEdf::_writeEdfHeader(aData,m_header,
					m_frame_per_file,
					buffer);
      const std::string& tmpBuffer = buffer.str();
      _compression(tmpBuffer.c_str(),tmpBuffer.size(),aBufferListPt);

      _compression((char*)aData.data(),aData.size(),aBufferListPt);
      _end_compression(aBufferListPt);
    }
  catch(Exception&)
    {
      DELETE_BUFFER_LIST(aBufferListPt);
      throw;
    }
  m_container._setBuffer(aData.frameNumber,aBufferListPt);
}

void FileZCompression::_compression(const char *buffer,int size,ZBufferType* return_buffers)
{
  DEB_MEMBER_FUNCT();
  
  m_compression_struct.next_in = (Bytef*)buffer;
  m_compression_struct.avail_in = size;
  
  while(m_compression_struct.avail_in)
    {
      TEST_AVAIL_OUT;
      if(deflate(&m_compression_struct,Z_NO_FLUSH) != Z_OK)
	THROW_CTL_ERROR(Error) << "deflate error";
      
      return_buffers->back()->used_size = _BufferHelper::BUFFER_HELPER_SIZE -
	m_compression_struct.avail_out;
    }
}
void FileZCompression::_end_compression(ZBufferType* return_buffers)
{
  DEB_MEMBER_FUNCT();
  
  int deflate_res = Z_OK;
  while(deflate_res == Z_OK)
    {
      TEST_AVAIL_OUT;
      deflate_res = deflate(&m_compression_struct,Z_FINISH);
      return_buffers->back()->used_size = _BufferHelper::BUFFER_HELPER_SIZE - 
	m_compression_struct.avail_out;
    }
  if(deflate_res != Z_STREAM_END)
    THROW_CTL_ERROR(Error) << "deflate error";
}
#endif // WITH_Z_COMPRESSION


#ifdef WITH_LZ4_COMPRESSION
FileLz4Compression::FileLz4Compression(CtSaving::SaveContainer &save_cnt,
			       int framesPerFile,const CtSaving::HeaderMap &header) :
  m_container(save_cnt),m_frame_per_file(framesPerFile),m_header(header)
{
  DEB_CONSTRUCTOR();
  
  LZ4F_errorCode_t result = LZ4F_createCompressionContext(&m_ctx, LZ4F_VERSION);
  if(LZ4F_isError(result))
    THROW_CTL_ERROR(Error) << "LZ4 context init failed: " << DEB_VAR1(result);
}

FileLz4Compression::~FileLz4Compression()
  {
    LZ4F_freeCompressionContext(m_ctx);
  }

void FileLz4Compression::process(Data &aData)
{
  DEB_MEMBER_FUNCT();
  DEB_PARAM() << DEB_VAR1(aData);
  
  std::ostringstream buffer;
  SaveContainerEdf::_writeEdfHeader(aData,m_header,
				    m_frame_per_file,
				    buffer);
  ZBufferType *aBufferListPt = new ZBufferType();
  const std::string& tmpBuffer = buffer.str();
  try
    {
      _compression(tmpBuffer.c_str(),tmpBuffer.size(),aBufferListPt);
      _compression((char*)aData.data(),aData.size(),aBufferListPt);
    }
  catch(Exception&)
    {
      DELETE_BUFFER_LIST(aBufferListPt);
      throw;
    }
  m_container._setBuffer(aData.frameNumber,aBufferListPt);
}

void FileLz4Compression::_compression(const char *src,int size,ZBufferType* return_buffers)
{
  DEB_MEMBER_FUNCT();
  
  int buffer_size = LZ4F_compressFrameBound(size,&lz4_preferences);
  buffer_size += LZ4_HEADER_SIZE + LZ4_FOOTER_SIZE;
  
  _BufferHelper *newBuffer = new _BufferHelper(buffer_size);
  return_buffers->push_back(newBuffer);
  char* buffer = (char*)newBuffer->buffer;
  
  int offset = LZ4F_compressBegin(m_ctx,buffer,
				  buffer_size,&lz4_preferences);
  if(LZ4F_isError(offset))
    THROW_CTL_ERROR(Error) << "Failed to start compression: " << DEB_VAR1(offset);

  int error_code = LZ4F_compressUpdate(m_ctx,buffer + offset,buffer_size - offset,
				       src,size,NULL);
  if(LZ4F_isError(error_code))
    THROW_CTL_ERROR(Error) << "Compression Failed: " 
			   << DEB_VAR2(error_code,LZ4F_getErrorName(error_code));
  offset += error_code;
  
  error_code = LZ4F_compressEnd(m_ctx, buffer + offset, size - offset, NULL);
  if(LZ4F_isError(error_code))
    THROW_CTL_ERROR(Error) << "Failed to end compression: " << DEB_VAR1(error_code);
  offset += error_code;
  newBuffer->used_size = offset;
}
#endif // WITH_LZ4_COMPRESSION

#ifdef WITH_BS_COMPRESSION

#include "bitshuffle.h"

extern "C" {
void bshuf_write_uint64_BE(void* buf, uint64_t num);
extern void bshuf_write_uint32_BE(void* buf, uint32_t num);
}


ImageBsCompression::ImageBsCompression(CtSaving::SaveContainer &save_cnt):
  m_container(save_cnt)
{
  DEB_CONSTRUCTOR();
  DEB_TRACE() << "BitShuffle using SSE2=" << bshuf_using_SSE2() << " AVX2=" << bshuf_using_AVX2();
};

ImageBsCompression::~ImageBsCompression()
{
}

void ImageBsCompression::process(Data &aData)
{
  ZBufferType *aBufferListPt = new ZBufferType;

  try
    {
      _compression((char*)aData.data(), aData.size(), aData.depth(), aBufferListPt);
    }
  catch(Exception&)
    {
      DELETE_BUFFER_LIST(aBufferListPt);
      throw;
    }
  m_container._setBuffer(aData.frameNumber,aBufferListPt);
}

void ImageBsCompression::_compression(const char *src,int data_size,int data_depth, ZBufferType* return_buffers)
{
  DEB_MEMBER_FUNCT();

  unsigned int bs_block_size= 0;
  unsigned int bs_in_size= (unsigned int)(data_size/data_depth);
  unsigned int bs_out_size;

  _BufferHelper *newBuffer = new _BufferHelper(data_size);
  char* bs_buffer = (char*)newBuffer->buffer;

  bshuf_write_uint64_BE(bs_buffer, data_size);
  bshuf_write_uint32_BE(bs_buffer+8, bs_block_size);
  bs_out_size = bshuf_compress_lz4(src, bs_buffer+12, bs_in_size, data_depth, bs_block_size);
  if (bs_out_size < 0)
    THROW_CTL_ERROR(Error) << "BS Compression failed: error code [" << bs_out_size << "]";
  else
    DEB_TRACE() << "BitShuffle Compression IN[" << data_size << "] OUT[" << bs_out_size << "]";

  return_buffers->push_back(newBuffer);
  return_buffers->back()->used_size = bs_out_size+12;
}

#endif // WITH_BS_COMPRESSION

#ifdef WITH_Z_COMPRESSION
ImageZCompression::ImageZCompression(CtSaving::SaveContainer &save_cnt,  int  level):
  m_container(save_cnt), m_compression_level(level)
{
  DEB_CONSTRUCTOR();
};

ImageZCompression::~ImageZCompression()
{
  ZBufferType *aBufferListPt = new ZBufferType;
}

void ImageZCompression::process(Data &aData)
{
  ZBufferType *aBufferListPt = new ZBufferType();
  
  try
    {
      _compression((char*)aData.data(),aData.size(),aBufferListPt);
    }
  catch(Exception&)
    {
      DELETE_BUFFER_LIST(aBufferListPt);
      throw;
    }
  m_container._setBuffer(aData.frameNumber,aBufferListPt);
}

void ImageZCompression::_compression(const char *src,int size,ZBufferType* return_buffers)
{
  DEB_MEMBER_FUNCT();
  uLong buffer_size;
  int status;
  // cannot know compression ratio in advance so allocate a buffer for full image size
  buffer_size = compressBound(size);
  _BufferHelper *newBuffer = new _BufferHelper(buffer_size);
  return_buffers->push_back(newBuffer);
  char* buffer = (char*)newBuffer->buffer;
  
  if ((status=compress2((Bytef*)buffer, &buffer_size, (Bytef*)src, size, m_compression_level)) < 0)
    THROW_CTL_ERROR(Error) << "Compression failed: error code " << status;
        
  return_buffers->back()->used_size = buffer_size;
}
#endif // WITH_Z_COMPRESSION

