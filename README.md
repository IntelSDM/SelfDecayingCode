# Self Decaying Code POC
Built for LLVM 19.1 - Windows x64 <br> <br>

Packing itself is mostly unnecessary in modern environments, attackers simply run your code, and dump it, to circumvent your extensively obfuscated packing system and entrypoint. <br>
Despite this being a known issue with packing for years now since offline DRMs became a thing of the past; no one really cared to punish people dumping until more recent advancements in [runtime packing systems; such as page encryption.](https://github.com/weak1337/NO_ACCESS_Protection) <br>
Even with modern packing mechanisms, everyone relies on runtime dumping still. So if they are going to runtime dump, a lot of one time only data such as for setting up hooks or loading a product can just be decayed at runtime. <br>
Sure you can VirtualProtect it with RWX and then delete the code, this would first of all give a syscall and a page fault to be tracked. <br>
Instead we give each opcode(They get deleted on use so nothing gets leaked) a unique handler in a R/W .data section and delete the opcode, removing your knowledge if you dump post execution. <br>
This is simply a POC and not for production use, It is simply to showcase how to make packing actually abrasive, and productive in modern code obfuscation. <br>

<img width="543" height="252" alt="image" src="https://github.com/user-attachments/assets/3bf138d3-dee0-4af0-9714-0f1a40ba16a8" />
<br>
<img width="812" height="848" alt="image" src="https://github.com/user-attachments/assets/6917a8c3-08e0-4e90-bddf-5797fd6531ee" />
<br>
As you can see undumped the opcodes are visible, it will execute fine, you can return the order perfectly fine: <br>
<img width="714" height="156" alt="image" src="https://github.com/user-attachments/assets/7cad9505-c52f-4ef9-96b7-ec2fbd0ac4e5" />
<br>
Dumped on the other hand: <br>
<img width="778" height="167" alt="image" src="https://github.com/user-attachments/assets/596b21fe-5a05-4e7f-9ed1-a70745f2beeb" />
<br>
All the data is now gone, So if you unpacked the binary naturally, you can simply retain order, but dumping it, it is gone, the code executed and now it is no longer needed. No page faults, no APIs or Syscalls, just gone.

## Annotations
```c
__attribute__((annotate("selfdecay")))
``` 
* Marks the function for self decay

## How To Build
* Download [LLVM 19.1](https://github.com/llvm/llvm-project/releases/tag/llvmorg-19.1.0)
* Download [VCPKG](https://github.com/microsoft/vcpkg)
* VCPKG install asmjit
* Open build.bat in a text editor.
* Find the LLVM 19.1 references in the command line, Change them to your file path.
* Find the VCPKG references in the command line, Change them to your file path.
* Run the build script
* Build your application with the ``` -load-pass-plugin ``` Param to the FunctionPass.dll
