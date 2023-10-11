import pandas as pd
import sys
import numpy as np

if (len(sys.argv) - 1) != 1:
    print("Error: no csv file prompted or more than 1 argument prompted!")
    exit()
dataframe = pd.read_csv(sys.argv[1])
x = range(1, len(dataframe.THROUGHPUT) + 1)
y1 = dataframe.THROUGHPUT
y2 = dataframe.BITRATE
z = dataframe.BUFFER
w = dataframe.QUALITY
sd = dataframe.STALLD
st = dataframe.STALLT

avg_bitrate = 0
for br in y2:
    avg_bitrate += br
avg_bitrate /= len(y2)
#print("The AVG Bitrate is: {:f} kbps".format(avg_bitrate))

std_bitrate = np.std(y2)
#print("The standard deviation for the Bitrate is: {:f} kbps".format(std_bitrate))

avg_quality = 0
for q in w:
    avg_quality += q + 1
avg_quality /= len(w)
#print("The AVG quality (1-7) is: {:f}".format(avg_quality))

n_down_switch = 0
instability = 0
for i in range(len(w) - 1):
    instability += abs(w[i] - w[i+1]) / (w[i+1] + 1)
    if w[i] > w[i+1]:
        n_down_switch += 1

#print("The number of downward switches is: {:f}".format(n_down_switch))
#print("The instability is: {:f}".format(instability))

n_stalls = -1
sd_temp = []
for i in range(len(sd)):
    if float(sd[i]) == 0.0:
        break
    n_stalls += 1
    sd_temp.append(sd[i])

#print("The number of stalls is: {:d}".format(n_stalls))
#print("The stall durations are:", end = " ")
#for stall in sd_temp:
#    print(stall, end = " ")
#print(" ")

print("Average bitrate (kbps),Average bitrate Std (kbps),Average quality level,Switches,Instability,Start-up (s),Stalls,Stall duration (s)")
print("{:.3f},{:.3f},{:.3f},{:.1f},{:.3f},{:.3f},{:.1f},".format(avg_bitrate,std_bitrate,avg_quality,n_down_switch,instability,sd_temp[0],n_stalls), end= "")
if len(sd_temp) > 2:
	print("={:.3f}".format(sd_temp[1]), end = "")
	for stall in sd_temp[2:]:
		print("+{:.3f}".format(stall), end = "")
elif len(sd_temp) > 1:
	print("{:.3f}".format(sd_temp[1]), end = "")
else:
	print("0.000", end = "")
print("")

#fig, (ax1, ax2, ax3, ax4) = plt.subplots(4, sharex=True)
#fig.suptitle(sys.argv[1])
#ax1.plot(x, y1, color = 'r', label = 'Throughput')
#ax1.plot(x, y2, color = 'g', label = 'Bitrate')
#ax1.legend(loc="upper right")
#ax2.plot(x, z, color = 'b', label = 'Buffer')
#ax2.legend(loc="upper right")
#ax3.plot(x, w, color = 'g', label = 'Quality')
#ax3.legend(loc="upper right")
#ax4.scatter(x, sd, color = 'y', label = 'Stall durations')
#ax4.scatter(x, st, color = 'k', label = 'Stall time')
#ax4.legend(loc="upper right")
#plt.plot(x, y, label = 'Throughput')
#plt.plot(x, z, label = 'Buffer')
#plt.plot(x, w, label = 'Quality')
#plt.title("Metrics 6")
#plt.xlabel("# segment")
#plt.show()  # or plt.savefig("name.png")
