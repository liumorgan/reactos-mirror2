<?xml version="1.0"?>
<rbuild xmlns:xi="http://www.w3.org/2001/XInclude">
	<module name="explorer" type="win32gui" installname="explorer2.exe" unicode="true">
		<include base="explorer">.</include>
		<define name="WIN32" />
		<define name="_WIN32_IE">0x0600</define>
		<define name="_WIN32_WINNT">0x0600</define>
		<define name="WINVER">0x0600</define>
		<library>advapi32</library>
		<library>kernel32</library>
		<library>gdi32</library>
		<library>user32</library>
		<library>comctl32</library>
		<library>msvcrt20</library>
		<library>ntdll</library>
		<library>ole32</library>
		<library>oleaut32</library>
		<library>shdocvw</library>
		<library>shell32</library>
		<library>shlwapi</library>
		<library>uuid</library>
		<pch>precomp.h</pch>
		<file>desktop.c</file>
		<file>dragdrop.c</file>
		<file>explorer.c</file>
		<file>startmnu.c</file>
		<file>taskband.c</file>
		<file>taskswnd.c</file>
		<file>tbsite.c</file>
		<file>trayntfy.c</file>
		<file>trayprop.c</file>
		<file>traywnd.c</file>
		<file>explorer.rc</file>
	</module>
</rbuild>
