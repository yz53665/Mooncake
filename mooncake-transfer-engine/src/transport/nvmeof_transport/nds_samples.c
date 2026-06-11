/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and limitations under the License.
==============================================================================*/

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "nds.h"
#include "acl_plugin.h"
#include "runtime_plugin.h"


#define MEM_SIZE (1024 * 1024)
#define DEVICE 2

int nds_file_read_write(int fd, int device, void *buf, size_t size, int offset)
{
    int ret = 0;
    nds_Handle nds_handle = nds_file_register(fd);
    if (nds_handle == NULL) {
        printf("nds_file_register error\n");
        ret = -1;
    } else {
        printf("nds_file_register success\n");
    }

    if (nds_read(nds_handle, DEVICE, buf, size, offset) < 0) {
        printf("nds_read error\n");
        ret = -1;
        goto file_deregiste;
    } else {
        printf("nds_read success\n");
    }
    
    if (nds_write(nds_handle, buf, size, offset) < 0){
        printf("nds_write error\n");
        ret = -1;
    } else {
        printf("nds_write success\n");
    }

file_deregiste:
    nds_file_deregister(fd);
    return ret;
}

int test()
{
    printf("test start:\n");
    char path[256];
    int size = sizeof(path);
    printf("size: %d \n", size);

    printf("test end:\n");
}

int main()
{
    test();
    printf("start main:\n");
    if (g_aclrtSetDevice(DEVICE) != 0) {
        printf("aclrtSetDevice error\n");
        return 0;
    } else {
        printf("aclrtSetDevice success\n");
    }
    if (nds_init(DEVICE) == -1) {
        printf("nds_init error\n");
        return 0;
    } else {
        printf("nds_init success\n");
    }
    void *buf = NULL;
    if (g_rtMalloc(&buf, MEM_SIZE, RT_MEMORY_HBM) != 0) {
        printf("rtMalloc error\n");
        return 0;
    } else {
        printf("rtMalloc success\n");
    }
    if (nds_buf_register(DEVICE, buf, MEM_SIZE) == -1) {
        printf("nds_buf_register error\n");
        return 0;
    } else {
        printf("nds_buf_register success\n");
    }

    char *file_path = "my_test.bin";
    int fd = open(file_path, O_RDWR);
    if (fd < 0) {
        printf("file to open file %s \n", file_path);
        return 0;
    }
    if (nds_file_read_write(fd, DEVICE, buf, MEM_SIZE, 0) < 0) {
        return 0;
    }

    close(fd);
    fd = -1;
    nds_buf_deregister(DEVICE, buf);
    nds_deinit(DEVICE);
    return 0;
}