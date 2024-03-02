Python 3.7.9 源码学习

目录介绍
Doc
官方文档的源码，也就是 http://docs.python.org/ 中的文档内容。参考如何构建文档

Grammar
用来放置 Python 的 EBNF 语法文件

Include
放置编译所需的全部头文件

Lib
标准库中的 Python 实现部分


Mac
Mac平台特定代码（比如 构建 OS X 的 IDLE 应用）

Misc
无法归类到其它地方的东西，通常是不同类型的特定开发者文档

Modules
标准库（还包括一些其它代码）中需要 C 实现的部分
os、math、random、io、sys、socket、re、opcode、gc、hashlib、ssl、time

Objects
所有内置类型的源码
内置类型有数字、序列、映射、类、实例和异常
数字（整数、浮点数、复数）
序列（列表、元组、range对象、字符串、字节串）
集合、字典
布尔值（True或False）
比较运算符
逻辑检测
迭代器
上下文管理器
模块
类和实例
方法
函数
代码对象
类型对象
空对象
省略符对象
未实现对象
内部对象

PC
Windows 平台特定代码

PCbuild
python.org 提供的 Windows 新版 MSVC 安装程序的构建文件

Parser
解析器相关代码，AST 节点的定义也在这里

Programs
可执行 C 程序的源码，包括 CPython 解释器的主函数（3.5版之前放在 Modules 目录）

Python
用来构建核心 CPython 运行时的代码，包括编译器、eval 循环和各种内置的模块。

Tools
用来维护 Python 的各种工具
