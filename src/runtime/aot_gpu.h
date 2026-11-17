#pragma once
/* Which Vulkan driver the game uses. 0 = the phone's own driver (the default, and what every build did before
   this existed); 1 = the bundled Turnip build for Adreno 6xx/7xx; 2 = the bundled Turnip build for Adreno 8xx.
   The choice is made in the menu that follows the folder picker and stored by the Java side; the native side only
   reads it and, if the driver does not come up on this phone, silently keeps the system driver. */
void aot_gpu_init(const char *native_lib_dir, int driver);   /* from Java (or from <ext>/driver.txt at boot) */
int  aot_gpu_apply(void);                                    /* call once, before DXVK creates its instance */
int  aot_gpu_driver(void);                                   /* the driver actually in effect: 0 = system */
