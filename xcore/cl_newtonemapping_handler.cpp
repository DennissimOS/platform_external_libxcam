/*
 * cl_newtonemapping_handler.cpp - CL tonemapping handler
 *
 *  Copyright (c) 2015 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *  Author: Wu Junkai <junkai.wu@intel.com>
 */
#include "xcam_utils.h"
#include "cl_newtonemapping_handler.h"

namespace XCam {

CLNewTonemappingImageKernel::CLNewTonemappingImageKernel (SmartPtr<CLContext> &context,
        const char *name)
    : CLImageKernel (context, name)
{
    _image_height = 540;

    for(int i = 0; i < 65536; i++)
    {
        _map_hist[i] = i;
    }
}

void Haleq(int *y, int *hist, int *hist_leq, int left, int right, int level, int index_left, int index_right)
{
    int l;
    float e, le;

    l = (left + right) / 2;
    int num_left = left > 0 ? hist[left - 1] : 0;
    int pixel_num = hist[right] - num_left;
    e = y[num_left + pixel_num / 2];
    le = 0.5f * (e - l) + l;
    int index = (index_left + index_right) / 2;
    hist_leq[index] = (int)(le + 0.5f);

    if(level > 6) return;

    Haleq(y, hist, hist_leq, left, (int)(le + 0.5f), level + 1, index_left, index);
    Haleq(y, hist, hist_leq, (int)(le + 0.5f) + 1, right, level + 1, index + 1, index_right);
}

XCamReturn
CLNewTonemappingImageKernel::prepare_arguments (
    SmartPtr<DrmBoBuffer> &input, SmartPtr<DrmBoBuffer> &output,
    CLArgument args[], uint32_t &arg_count,
    CLWorkSize &work_size)
{
    SmartPtr<CLContext> context = get_context ();

    _image_in = new CLVaImage (context, input);
    _image_out = new CLVaImage (context, output);

    const VideoBufferInfo & in_video_info = input->get_video_info ();
    _image_height = in_video_info.aligned_height;

    XCAM_ASSERT (_image_in->is_valid () && _image_out->is_valid ());
    XCAM_FAIL_RETURN (
        WARNING,
        _image_in->is_valid () && _image_out->is_valid (),
        XCAM_RETURN_ERROR_MEM,
        "cl image kernel(%s) in/out memory not available", get_kernel_name ());

    SmartPtr<X3aStats> stats = input->find_3a_stats ();
    XCam3AStats *stats_ptr = stats->get_stats ();
    int stats_totalnum = stats_ptr->info.width * stats_ptr->info.height;
    int hist_bin_count = 1 << stats_ptr->info.bit_depth;
    int gain = 65536 / hist_bin_count;
    int y_max;

    for(int i = hist_bin_count - 1; i >= 0; i--)
    {
        if(stats_ptr->hist_y[i] > 0)
        {
            y_max = i;
            break;
        }
    }

    int* hist_new = new int[65536];
    int* hist_log = new int[hist_bin_count];
    int* sort_y = new int[stats_totalnum];
    float* map_index_leq = new float[65536];
    int* map_index_log = new int[65536];

    float t = 0.01f * y_max;
    float max_log = log(y_max + t);
    float t_log = log(t);
    for(int i = 0; i < hist_bin_count; i++)
    {
        hist_log[i] = 0;
    }

    float factor = (hist_bin_count - 1) / (max_log - t_log);
    for(int i = 0; i < y_max; i++)
    {
        int index = (int)((log(i + t) - t_log) * factor + 0.5f);
        hist_log[index] += stats_ptr->hist_y[i];
        for(int l = 0; l < gain; l++)
        {
            map_index_log[l + i * gain] = l + index * gain;
        }
    }

    for(int i = y_max * gain; i < 65536; i++)
    {
        map_index_log[i] = 65535;
    }

    for(int i = 0; i < hist_bin_count; i++)
    {
        int avg_bin = hist_log[i] / gain;
        int remain_num = hist_log[i] % gain;

        for(int l = 0; l < gain; l++)
        {
            hist_new[l + i * gain] = avg_bin + (l < remain_num ? 1 : 0);
        }
    }

    int sort_index = 0;
    for(int i = 0; i < 65536; i++)
    {
        for(int l = 0; l < hist_new[i]; l++)
        {
            sort_y[sort_index] = i;
            sort_index++;
        }
    }

    for(int i = 1; i < 65536; i++)
    {
        hist_new[i] += hist_new[i - 1];
    }

    int map_leq_index[256];

    for(int i = 0; i < 256; i++)
    {
        map_leq_index[i] = i;
    }

    Haleq(sort_y, hist_new, map_leq_index, 0, 65535, 0, 0, 255);

    map_leq_index[255] = 65536;

    for(int i = 1; i < 256; i++)
    {
        if(map_leq_index[i] < map_leq_index[i - 1])
            map_leq_index[i] = map_leq_index[i - 1];
    }

    int k;
    for(int i = 0; i < 255; i++)
    {
        for(k = map_leq_index[i]; k < map_leq_index[i + 1]; k++)
        {
            map_index_leq[k] = i;
        }
    }

    for(int i = 0; i < 65536; i++)
    {
        _map_hist[i] = map_index_leq[map_index_log[i]] / 255.0f;
    }

    delete[] hist_new;
    delete[] hist_log;
    delete[] map_index_leq;
    delete[] map_index_log;
    delete[] sort_y;

    _map_hist_buffer = new CLBuffer(
        context, sizeof(float) * 65536,
        CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, &_map_hist);

    //set args;
    args[0].arg_adress = &_image_in->get_mem_id ();
    args[0].arg_size = sizeof (cl_mem);
    args[1].arg_adress = &_image_out->get_mem_id ();
    args[1].arg_size = sizeof (cl_mem);
    args[2].arg_adress = &_map_hist_buffer->get_mem_id ();
    args[2].arg_size = sizeof (cl_mem);
    args[3].arg_adress = &_image_height;
    args[3].arg_size = sizeof (int);

    arg_count = 4;

    const CLImageDesc out_info = _image_out->get_image_desc ();
    work_size.dim = XCAM_DEFAULT_IMAGE_DIM;
    work_size.global[0] = out_info.width;
    work_size.global[1] = out_info.height / 4;
    work_size.local[0] = 8;
    work_size.local[1] = 8;

    return XCAM_RETURN_NO_ERROR;
}

CLNewTonemappingImageHandler::CLNewTonemappingImageHandler (const char *name)
    : CLImageHandler (name)
    , _output_format (XCAM_PIX_FMT_SGRBG16_planar)
{
}

bool
CLNewTonemappingImageHandler::set_tonemapping_kernel(SmartPtr<CLNewTonemappingImageKernel> &kernel)
{
    SmartPtr<CLImageKernel> image_kernel = kernel;
    add_kernel (image_kernel);
    _tonemapping_kernel = kernel;
    return true;
}

XCamReturn
CLNewTonemappingImageHandler::prepare_buffer_pool_video_info (
    const VideoBufferInfo &input,
    VideoBufferInfo &output)
{
    bool format_inited = output.init (_output_format, input.width, input.height);

    XCAM_FAIL_RETURN (
        WARNING,
        format_inited,
        XCAM_RETURN_ERROR_PARAM,
        "CL image handler(%s) ouput format(%s) unsupported",
        get_name (), xcam_fourcc_to_string (_output_format));

    return XCAM_RETURN_NO_ERROR;
}


SmartPtr<CLImageHandler>
create_cl_newtonemapping_image_handler (SmartPtr<CLContext> &context)
{
    SmartPtr<CLNewTonemappingImageHandler> tonemapping_handler;
    SmartPtr<CLNewTonemappingImageKernel> tonemapping_kernel;
    XCamReturn ret = XCAM_RETURN_NO_ERROR;

    tonemapping_kernel = new CLNewTonemappingImageKernel (context, "kernel_newtonemapping");
    {
        XCAM_CL_KERNEL_FUNC_SOURCE_BEGIN(kernel_newtonemapping)
#include "kernel_newtonemapping.clx"
        XCAM_CL_KERNEL_FUNC_END;
        ret = tonemapping_kernel->load_from_source (kernel_newtonemapping_body, strlen (kernel_newtonemapping_body));
        XCAM_FAIL_RETURN (
            WARNING,
            ret == XCAM_RETURN_NO_ERROR,
            NULL,
            "CL image handler(%s) load source failed", tonemapping_kernel->get_kernel_name());
    }
    XCAM_ASSERT (tonemapping_kernel->is_valid ());
    tonemapping_handler = new CLNewTonemappingImageHandler("cl_handler_newtonemapping");
    tonemapping_handler->set_tonemapping_kernel(tonemapping_kernel);

    return tonemapping_handler;
}

};
