cmd.exe /c "taskkill /IM np21x64w.exe /F" 2>/dev/null
sleep 2
cmd.exe /c "copy C:\WATCOM\src\os32_gcc\os.d88 C:\Users\hight\OneDrive\ドキュメント\np21w\os.d88 /Y"
powershell.exe -c "Start-Process 'C:\Users\hight\OneDrive\ドキュメント\np21w\np21x64w.exe' -WorkingDirectory 'C:\Users\hight\OneDrive\ドキュメント\np21w'"
echo "Waiting 5 sec for emulator launch"
sleep 5
python3 /mnt/c/WATCOM/src/os32_gcc/tools/rcmd.py --wait-boot 60
python3 install_auto.py > install.log 2>&1
