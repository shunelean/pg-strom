/*
 * opencl_gpuscan.h
 *
 * OpenCL device code specific to GpuScan logic
 * --
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#ifndef OPENCL_GPUSCAN_H
#define OPENCL_GPUSCAN_H

/*
 * Sequential Scan using GPU/MIC acceleration
 *
 * It packs a kern_parambuf and kern_resultbuf structure within a continuous
 * memory ares, to transfer (usually) small chunk by one DMA call.
 *
 * +----------------+       -----
 * | kern_parambuf  |         ^
 * | +--------------+         |
 * | | length   o--------------------+
 * | +--------------+         |      | kern_vrelation is located just after
 * | | nparams      |         |      | the kern_parambuf (because of DMA
 * | +--------------+         |      | optimization), so head address of
 * | | poffset[0]   |         |      | kern_gpuscan + parambuf.length
 * | | poffset[1]   |         |      | points kern_resultbuf.
 * | |    :         |         |      |
 * | | poffset[M-1] |         |      |
 * | +--------------+         |      |
 * | | variable     |         |      |
 * | | length field |         |      |
 * | | for Param /  |         |      |
 * | | Const values |         |      |
 * | |     :        |         |      |
 * +-+--------------+  -----  |  <---+
 * | kern_vrelation |    ^    |
 * | +--------------+    |    |  Area to be sent to OpenCL device.
 * | | nrels        |    |    |  Forward DMA shall be issued here.
 * | +--------------+    |    |
 * | | nitems       |    |    |
 * | +--------------+    |    |
 * | | nrooms (=N)  |    |    |
 * | +--------------+    |    |
 * | | errcode      |    |    V
 * | +--------------+    |  -----
 * | | rindex[0]    |    |
 * | | rindex[1]    |    |  Area to be written back from OpenCL device.
 * | |     :        |    |  Reverse DMA shall be issued here.
 * | | rindex[N-1]  |    V
 * +-+--------------+  -----
 *
 * Gpuscan kernel code assumes all the fields shall be initialized to zero.
 */
typedef struct {
	kern_parambuf	kparams;
	/*
	 * as above, kern_resultbuf shall be located next to the parambuf
	 */
} kern_gpuscan;

#define KERN_GPUSCAN_PARAMBUF(kgpuscan)			\
	((__global kern_parambuf *)(&(kgpuscan)->kparams))
#define KERN_GPUSCAN_LENGTH(kgpuscan)			\
	(KERN_GPUSCAN_PARAMBUF(kgpuscan)->length)

#ifdef OPENCL_DEVICE_CODE
/*
 * gpuscan_writeback_row_error
 *
 * It writes back the calculation result of gpuscan.
 */
static void
gpuscan_writeback_row_error(__global kern_vrelation *kvrel,
							int errcode,
							__local void *workmem)
{
	__local cl_uint	base;
	cl_uint		binary;
	cl_uint		offset;
	cl_uint		nitems;

	/*
	 * A typical usecase of arithmetic_stairlike_add with binary value:
	 * It takes 1 if thread wants to return a status to the host side,
	 * then stairlike-add returns a relative offset within workgroup,
	 * and we can adjust this offset by global base index.
	 */
	binary = (get_global_id(0) < kvrel->nrooms &&
			  (errcode == StromError_Success ||
			   errcode == StromError_RowReCheck)) ? 1 : 0;

	offset = arithmetic_stairlike_add(binary, workmem, &nitems);

	if (get_local_id(0) == 0)
		base = atomic_add(&kresbuf->nitems, nitems);
	barrier(CLK_LOCAL_MEM_FENCE);

	/*
	 * Write back the row-index that passed evaluation of the qualifier,
	 * or needs re-check on the host side. In case of re-check, row-index
	 * shall be a negative number.
	 */
	if (get_global_id(0) >= kvrel->nrooms)
		return;

	if (errcode == StromError_Success)
		kresbuf->results[base + offset] = (get_global_id(0) + 1);
	else if (errcode == StromError_RowReCheck)
		kresbuf->results[base + offset] = -(get_global_id(0) + 1);
}

/*
 * forward declaration of the function to be generated on the fly
 */
static pg_bool_t
gpuscan_qual_eval(__private cl_int *errcode,
				  __global kern_parambuf *kparams,
				  __global kern_vrelation *kvrel,
				  __global kern_data_store *kds,
				  __global kern_toastbuf *ktoast,
				  size_t kds_index);
/*
 * kernel entrypoint of gpuscan
 */
__kernel void
gpuscan_qual(__global kern_gpuscan *kgpuscan,
			 __global kern_vrelation *kvrel,
			 __global kern_data_store *kds,
			 __global kern_toastbuf *ktoast,
			 __local void *local_workbuf)
{
	pg_bool_t	rc;
	cl_int		errcode = StromError_Success;
	__global kern_parambuf *kparams = KERN_GPUSCAN_PARAMBUF(kgpuscan);

	if (get_global_id(0) < kcs->nrows)
		rc = gpuscan_qual_eval(&errcode,
							   kparams, kvrel, kds, ktoast,
							   get_global_id(0));
	else
		rc.isnull = true;

	STROM_SET_ERROR(&errcode,
					!rc.isnull && rc.value != 0
					? StromError_Success
					: StromError_RowFiltered);

	/* writeback error code */
	kern_writeback_error_status(&kvrel->errcode, errcode, local_workbuf);
	gpuscan_writeback_row_error(kvrel, errcode, local_workmem);
}

#if 0
/*
 * kernel entrypoint of gpuscan for column-store
 */
__kernel void
gpuscan_qual_cs(__global kern_gpuscan *kgscan,
				__global kern_column_store *kcs,
				__global kern_toastbuf *toast,
				__local void *local_workmem)
{
	pg_bool_t	rc;
	cl_int		errcode = StromError_Success;
	__global kern_parambuf *kparams = KERN_GPUSCAN_PARAMBUF(kgscan);

	if (get_global_id(0) < kcs->nrows)
		rc = gpuscan_qual_eval(&errcode, kgscan, kcs, toast, get_global_id(0));
	else
		rc.isnull = true;

	STROM_SET_ERROR(&errcode,
					!rc.isnull && rc.value != 0
					? StromError_Success
					: StromError_RowFiltered);
	gpuscan_writeback_result(kgscan, errcode, local_workmem);
}

/*
 * kernel entrypoint of gpuscan for row-store
 */
__kernel void
gpuscan_qual_rs(__global kern_gpuscan *kgscan,
				__global kern_row_store *krs,
				__global kern_column_store *kcs,
				__local void *local_workmem)
{
	__global kern_parambuf *kparams = KERN_GPUSCAN_PARAMBUF(kgscan);
	cl_int				errcode = StromError_Success;
	pg_bytea_t			kparam_0 = pg_bytea_param(kparams,&errcode,0);
	pg_bool_t			rc;
	__local size_t		kcs_offset;
	__local size_t		kcs_nitems;

	/* acquire a slot of this workgroup */
	if (get_local_id(0) == 0)
	{
		if (get_global_id(0) + get_local_size(0) < krs->nrows)
			kcs_nitems = get_local_size(0);
		else if (get_global_id(0) < krs->nrows)
			kcs_nitems = krs->nrows - get_global_id(0);
		else
			kcs_nitems = 0;
		kcs_offset = atomic_add(&kcs->nrows, kcs_nitems);
	}
	barrier(CLK_LOCAL_MEM_FENCE);

	/* move data into column-store */
	kern_row_to_column(&errcode,
					   (__global cl_char *)VARDATA(kparam_0.value),
					   krs,
					   get_global_id(0),
					   kcs,
					   NULL,
					   kcs_offset,
					   kcs_nitems,
					   local_workmem);
	if (get_local_id(0) < kcs_nitems)
	{
		rc = gpuscan_qual_eval(&errcode, kgscan, kcs,
							   (__global kern_toastbuf *)krs,
							   kcs_offset + get_local_id(0));
	}
	else
		rc.isnull = true;

	STROM_SET_ERROR(&errcode,
					!rc.isnull && rc.value != 0
					? StromError_Success
					: StromError_RowFiltered);
	gpuscan_writeback_result(kgscan, errcode, local_workmem);
}
#endif

#else	/* OPENCL_DEVICE_CODE */

/*
 * Host side representation of kern_gpuscan. It has a program-id to be
 * executed on the OpenCL device, and either of row- or column- store
 * to be processed, in addition to the kern_gpuscan buffer including
 * kern_parambuf for constant values.
 */
typedef struct {
	pgstrom_message		msg;		/* = StromTag_GpuScan */
	Datum				dprog_key;	/* key of device program */
	StromObject		   *rc_store;	/* = StromTag_TCache(Row|Column)Store */
	pgstrom_vrelation  *vrel;		/* = StromTag_VirtRelation */
	kern_gpuscan		kern;
} pgstrom_gpuscan;

#endif	/* OPENCL_DEVICE_CODE */
#endif	/* OPENCL_GPUSCAN_H */
