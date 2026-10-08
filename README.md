# 工作咩闹钟 做Window电脑显示器的上位机

可以将任意显示器画面同步到任意Linux设备上，比如 Buildroot的智能音箱，Kindle，KVM或路由器上的小屏幕  
无需任何依赖，直接写drm或fb  
只需要电脑5%的CPU和2%的GPU，远低于Sunshine和Spacedesk

## 如何使用
直接双击运行也行

```cmd
displayPost.exe <url> [fps=30] [monitor=1] [rotation=0] [color=0]
```

- `url`: 工作咩闹钟Web后台地址，比如 `http://192.168.1.154:8080`
- `fps`: 每秒帧率，默认 `30`
- `monitor`: Windows的显示器编号，默认 `1`
- `rotation`: 屏幕旋转，0/90/180/270，默认不转
- `color`: 色彩处理模式：0 自动（默认），1 彩色，2 灰度，3 Floyd-Steinberg 黑白抖动，4 Bayer 4x4 黑白抖动，5 二值化黑白

比如:
```cmd
displayPost.exe http://192.168.2.195:8080/ 30 1 90
```


