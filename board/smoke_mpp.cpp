// 交叉编译冒烟测试:验证 MPP + RGA 头文件/库链接通过、板端可运行
#include <stdio.h>
#include <cstring>
extern "C" {
#include "rk_mpi.h"
}
#include "rga.h"
#include "im2d.h"

int main() {
    MppCtx ctx = nullptr;
    MppApi *mpi = nullptr;
    MPP_RET ret = mpp_create(&ctx, &mpi);
    printf("mpp_create: ret=%d\n", ret);
    if (ret == MPP_OK) {
        ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
        printf("mpp_init(JPEG dec): ret=%d\n", ret);
        mpp_destroy(ctx);
    }

    // RGA im2d: 查询硬件版本/能力,验证驱动和链接 OK
    const char *ver = querystring(RGA_VERSION);
    printf("RGA version: %s\n", ver ? ver : "(null)");
    return (ret == MPP_OK) ? 0 : 1;
}
