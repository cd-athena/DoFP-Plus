# from operator import index
# from tracemalloc import start
# from turtle import down
# from distutils.command.build import build
# from ast import Break
import numpy as np

available_bitrates = [0, 107, 240, 346, 715, 1347, 2426, 4121]
num_quality_levels = len(available_bitrates)
available_qualities = [i for i in range(num_quality_levels)]
quality_array = [6, 5, 5, 6, 6,
                 0]  # [900, 300, 300, 1600, 900, 2400, 300, 0] # first element is playing segment [1, 3, 1, 4, 3, 5, 1, 0]
# gap_array = [300, 300, 900]
available_times = []  # # first element is playing segment

buffer_size = 20
estimated_throughput = 3634
current_buffer = 16.743
# Minh - increase avg quality - add - S
high_buffer_threshold = 0.75*buffer_size
isMaxAvgQuality = True
# Minh - increase avg qualit - add - E
save_buffer_threshold = 0.5 * buffer_size
low_buffer_threshold = 0.25 * buffer_size
total_remaining_time = current_buffer - save_buffer_threshold
segment_duration = 4
remaining_time_current_segment = 1
available_time_next_segment = 4
retrans_strategy = 1  # 1 means download segments in 1 gaps only. 0 means download all gaps at once


# find gaps
def get_gaps(input_array):
    print("== GET GAPS == input array: {}".format(input_array))
    gap_quality_array = []
    gap_index_array = []
    target_quality_array = []  # from 1-higher-level -> max quality of neighbors.
    length = len(input_array)
    start_gap_index = -1
    end_gap_index = -1
    target_quality = 0
    for i in np.arange(1, length):
        if input_array[i] < input_array[i - 1]:
            if (end_gap_index == -1 and start_gap_index == -1) or \
                    (end_gap_index != -1):  # and start_gap_index != -1):
                print("start: {}".format(i))
                start_gap_index = i
                end_gap_index = -1
            else:
                if len(gap_quality_array):
                    print("delete the last element: {}".format(i))
                    gap_quality_array.pop()
                    gap_index_array.pop()

            gap_quality_array.append([input_array[i]])
            gap_index_array.append([i])
            target_quality = input_array[i - 1]
            if (i == length - 2):  # current segment
                print("last: {}".format(i))
                target_quality_array.append(input_array[i - 1])
                start_gap_index = i
                end_gap_index = i
            if (i == length - 1):  # last segment
                print("last: {}".format(i))
                # target_quality_array.append(input_array[i-1])
                target_quality_array.append(num_quality_levels - 1)  
                start_gap_index = i
                end_gap_index = -1              

        elif input_array[i] == input_array[i - 1]:
            if start_gap_index != -1:
                print("continue: {}".format(i))
                gap_quality_array[len(gap_quality_array) - 1].append(input_array[i])
                gap_index_array[len(gap_quality_array) - 1].append(i)
        else:
            print("end: {}".format(i))
            end_gap_index = i - 1
            if start_gap_index != -1:
                target_quality = min(target_quality, input_array[i])
                target_quality_array.append(target_quality)
                start_gap_index = -1

    print("=> Gap's quality: {}".format(gap_quality_array))
    print("=> Gap's index: {}".format(gap_index_array))
    print("==> Target quality: {}".format(target_quality_array))
    return gap_quality_array, gap_index_array, target_quality_array


def get_extended_gaps(input_array):
    print("== get_extended_gaps == input array: {}".format(input_array))
    length = len(input_array)    
    gap_quality_array = []
    gap_index_array = []
    target_quality_array = [num_quality_levels - 1]  # from 1-higher-level -> max quality of neighbors.
    target_quality_array[0] = num_quality_levels - 1

    start_gap_index = -1
    end_gap_index = -1
    target_quality = 0
    print(gap_quality_array)
    print(gap_index_array)
    # find continuous segments with the same quality
    gap_index_array.append([1])
    gap_quality_array.append([input_array[1]])

    for i in np.arange(2, length-1):
        if input_array[i] == input_array[i - 1]:    # continue a group of same-quality segment
            gap_index_array[len(gap_index_array) - 1].append(i)
            gap_quality_array[len(gap_quality_array) - 1].append(input_array[i])
        else:                                       # start a new group
            gap_index_array.append([i])
            gap_quality_array.append([input_array[i]])

    print("---------grouping segments")
    print(gap_quality_array)
    print(gap_index_array)    
    final_gap_quality_array = []
    final_gap_index_array = []
    # determine segment index and target quality of each gap
    if (gap_quality_array[0][0] <  gap_quality_array[1][0]):
        final_gap_quality_array.append(gap_quality_array[0][:])

    # return output as same as find_gaps()     
    for i in np.arange(1, length-1):
        print("---- i: {}".format(i))
        if input_array[i] < input_array[i - 1]: # extend the segments with lowest quality as gaps
            if (end_gap_index == -1 and start_gap_index == -1) or \
                    (end_gap_index != -1):  # and start_gap_index != -1):
                print("start: {}".format(i))
                start_gap_index = i
                end_gap_index = -1
                
                if i == length-2:
                    target_quality_array.append(num_quality_levels - 1)
            else:
                if len(gap_quality_array):
                    print("delete the last element: {}".format(i))
                    gap_quality_array.pop()
                    gap_index_array.pop()

            gap_quality_array.append([input_array[i]])
            gap_index_array.append([i])
            target_quality = input_array[i - 1]
            # if (i == length - 1):  # last segment
            #     print("last: {}".format(i))
            #     # target_quality_array.append(input_array[i-1])
            #     target_quality_array.append(num_quality_levels - 1)

        elif input_array[i] == input_array[i - 1]:
            if start_gap_index != -1:
                print("continue: {}".format(i))
                gap_quality_array[len(gap_quality_array) - 1].append(input_array[i])
                gap_index_array[len(gap_quality_array) - 1].append(i)
            if i == length-2:
                target_quality_array.append(num_quality_levels - 1)
        else:
            print("end: {}".format(i))
            end_gap_index = i - 1
            if start_gap_index != -1:
                target_quality = max(target_quality, input_array[i])
                target_quality_array.append(target_quality)
                start_gap_index = -1

    print("=> Gap's quality: {}".format(gap_quality_array))
    print("=> Gap's index: {}".format(gap_index_array))
    print("==> Target quality: {}".format(target_quality_array))
    return gap_quality_array, gap_index_array, target_quality_array    


# sort the quality array
# smallest quality -> available time ->  narrowest gap
# in a gap, download from the last segment then move backward
def get_ordered_gaps(gap_quality_array, gap_index_array, target_quality_array):
    print("get_ordered_gaps")

    # smallest gap
    # gap_size_array = [len(x) for x in gap_quality_array]

    # gap_size_array_sorted = np.sort(gap_size_array)
    # index_sorted = np.argsort(gap_size_array)
    # gap_quality_array_sorted = [gap_quality_array[i] for i in index_sorted]
    # gap_index_array_sorted = [gap_index_array[i] for i in index_sorted]
    # target_quality_array_sorted = [target_quality_array[i] for i in index_sorted]

    # smallest quality -> smallest available time
    gap_quality = [x[0] for x in gap_quality_array]
    print(gap_quality)
    gap_quality_sorted = np.sort(gap_quality)
    index_sorted = np.argsort(gap_quality)
    gap_quality_array_sorted = [gap_quality_array[i] for i in index_sorted]
    gap_index_array_sorted = [gap_index_array[i] for i in index_sorted]
    target_quality_array_sorted = [target_quality_array[i] for i in index_sorted]
    print("-- gap_quality_sorted: {}".format(gap_quality_sorted))
    print("-- gap_quality_array_sorted: {}".format(gap_quality_array_sorted))
    print("-- gap_index_array_sorted: {}".format(gap_index_array_sorted))
    print("-- target_quality_array_sorted: {}".format(target_quality_array_sorted))

    return gap_index_array_sorted, target_quality_array_sorted


def get_objective_function(m_quality_array):
    factor_1 = sum(m_quality_array) / (len(m_quality_array) * num_quality_levels)

    num_downward = 0
    num_upwnward = 0
    factor_2 = 0
    factor_3 = 0
    alpha = 0.8
    for current_quality, next_quality in zip(m_quality_array[:-1], m_quality_array[1:]):
        factor_2 += abs((current_quality - next_quality) / (len(m_quality_array) * (next_quality)))
    #     if current_quality > next_quality:
    #         factor_2 += (current_quality - next_quality) / (len([1]) * (next_quality + 1))
    #         num_downward += 1
    #     # Minh - add - S
    #     elif current_quality < next_quality:
    #         factor_3 += (current_quality - next_quality) / (len([1]) * (next_quality + 1))
    #         num_upwnward += 1
    #     # Minh - add - E

    # if num_downward:
    #     factor_2 = factor_2 / len(m_quality_array)

    # if num_upwnward:
    #     factor_3 = factor_3 / len(m_quality_array)

    # print("** factor_1: {}. factor_2: {}. factor_3: {}".format(factor_1, factor_2, factor_3))

    # Minh - modify - S
    return alpha * factor_1 - (1 - alpha) * factor_2  # + 0.5*factor_3
    # Minh - modify - S


def initialize_run_dofp(seg_bitrates, qualities, throughput, buffer, save_buffer_t, seg_length, time_current_segment,
                        time_next_segment, strategy):
    print("Executing function initialize_run_dofp!")
    # print("seg_bitrates={}, qualities={}, throughput={:.3f}, buffer={:.3f}, save_buffer_threshold={:.3f}, seg_length={:d}, time_current_segment={:.3f}, time_next_segment={":.3f"}, retransmission_strategy={:d}\n".format(seg_bitrates, qualities, throughput, buffer, save_buffer_t, seg_length, time_current_segment, time_next_segment, strategy))
    global available_bitrates, num_quality_levels, available_qualities, quality_array, estimated_throughput, current_buffer, save_buffer_threshold, total_remaining_time, segment_duration, remaining_time_current_segment, available_time_next_segment, retrans_strategy
    # Bitrates assignment
    available_bitrates = []
    for i in range(len(seg_bitrates)):
        available_bitrates.append(seg_bitrates[i])
    num_quality_levels = len(available_bitrates)
    # Available quality assignment
    # available_qualities = []
    available_qualities = [i for i in range(num_quality_levels)]
    # Quality array assignment
    quality_array = []
    for i in range(len(qualities) - 1):
        quality_array.append(qualities[i] + 1)
    quality_array.append(0)
    # Estimated throughput
    estimated_throughput = throughput
    # Current buffer
    current_buffer = buffer
    # Save buffer threshold
    save_buffer_threshold = save_buffer_t
    # Total remaining time
    total_remaining_time = current_buffer - save_buffer_threshold
    # Segment duration
    segment_duration = seg_length
    # Remaining time current segment
    remaining_time_current_segment = time_current_segment
    # Available time next segment
    available_time_next_segment = time_next_segment
    # Retransmission strategy
    retrans_strategy = strategy
    # Check variables
    print(
        "available_bitrates={}, available_qualities={}, quality_array={}, estimated_throughput={:.3f}, current_buffer={:.3f}, save_buffer_threshold={:.3f}, total_remaining_time={:.3f}, segment_duration={:d}, remaining_time_current_segment={:.3f}, available_time_next_segment={:.3f}, retrans_strategy={:d}\n".format(
            available_bitrates, available_qualities, quality_array, estimated_throughput, current_buffer,
            save_buffer_threshold, total_remaining_time, segment_duration, remaining_time_current_segment,
            available_time_next_segment, retrans_strategy))
    # RUN DOFP #
    return run_dofp()


def get_min_objective_value_quality_array(quality_array, available_download_time, estimated_throughput):
    max_objective_value = -1000000
    selected_quality_array = quality_array[:]
    selected_quality_array[-1] = 1    
    for q_n in available_qualities[1:]:
        temp_quality_array = quality_array[:]
        temp_quality_array[-1] = (q_n)
        print("\t\t-------- temp_quality_array  = {}".format(temp_quality_array))
        current_objective_value = get_objective_function(temp_quality_array)
        if max_objective_value < current_objective_value and \
                available_bitrates[q_n] * segment_duration / estimated_throughput < available_download_time:
            print("\t\t\t-------==> objective_value: {}".format(current_objective_value))

            # new_quality_array = temp_quality_array[:]
            max_objective_value = current_objective_value
            selected_quality_array = temp_quality_array[:]
            print("\t\t\t====== selected_quality_array: {}".format(selected_quality_array))
            print("\t\t\t====== consider_objective_value: {}".format(selected_quality_array))
    print("========= Selected: {}".format(selected_quality_array))
    return selected_quality_array, max_objective_value


def run_dofp():
    global available_times
    # OPTIMIZATION MODEL #
    if quality_array[-1] != 0:
        print("ERROR: The quality of the last segment is not 0")
        return quality_array[:]
    else:
        print("============================= DOFP+ =================================")
        num_quality_levels = len(available_bitrates)
        gap_quality_array, gap_index_array, target_quality_array = get_gaps(quality_array)
        # order gap quality
        gap_index_array_sorted, target_quality_array_sorted = get_ordered_gaps(gap_quality_array, gap_index_array,
                                                                               target_quality_array)
        available_times = [
            [remaining_time_current_segment + (i - 1) * segment_duration for i in gap_index_array_sorted[j]] for j in
            range(len(gap_index_array_sorted))]

        # Minh change available time of next segment - S
        if current_buffer < save_buffer_threshold:
            print("\t current_buffer={}. save_buffer_threshold={}".format(current_buffer, save_buffer_threshold))
            available_time_next_segment_m = segment_duration
            # Calculate objective value if only next segment is downloaded
            selected_quality_array, max_objective_value = get_min_objective_value_quality_array(quality_array,
                                                                                                available_time_next_segment_m,
                                                                                                estimated_throughput)
            return selected_quality_array
        else:
            if isMaxAvgQuality and current_buffer > high_buffer_threshold:
                available_time_next_segment_m = current_buffer - save_buffer_threshold
            else:
                available_time_next_segment_m = segment_duration  # max(0, segment_duration + (current_buffer - save_buffer_threshold))
            print("-----> available_time_next_segment_m: {}".format(available_time_next_segment_m))
        # Minh change available time of next segment - E

        # Minh - update abr algorithm - S

        if len(gap_index_array_sorted) == 1:  # no gap in the buffer
            selected_quality_array, max_objective_value = get_min_objective_value_quality_array(quality_array,
                                                                                                available_time_next_segment_m,
                                                                                                estimated_throughput)
            return selected_quality_array
        else:  # there are some gaps
            if available_times:
                print("\t available time of next segment (1): {}".format(segment_duration))
                available_times[0] = [segment_duration]
            else:
                print("\t available time of next segment (2): {}".format(segment_duration))
                available_times.append([segment_duration])

            consider_quality_array = [-100000] * len(target_quality_array)
            consider_objective_value = [-100000] * len(target_quality_array)
            max_objective_value = -10000

            new_quality_array = quality_array[:]

            t_download = []
            next_gap_flag = False

            print("----> available_time: {}".format(available_times))
            print("----> new_quality_array: {}".format(new_quality_array))

            isRetransPossible = False
            for q_n in available_qualities[1:]:  # r_n is the quality for the next segment
                t_download = [available_bitrates[q_n] * segment_duration / estimated_throughput]
                print("*************************************************\n==> in quality {}, t_download {}".format(
                    q_n, t_download))
                if t_download[0] > available_times[0][0]:
                    break

                print("---. First t_total: {:.1f}".format(t_download[-1]))
                # Calculate objective value if only next segment is downloaded
                # temp_quality_array = new_quality_array[:]
                temp_quality_array = quality_array[:]
                temp_quality_array[-1] = (q_n)
                print("-------- temp_quality_array  = {}".format(temp_quality_array))
                current_objective_value = get_objective_function(temp_quality_array)
                if max_objective_value < current_objective_value:
                    print("\t\t\t-------==> objective_value: {}".format(current_objective_value))

                    # new_quality_array = temp_quality_array[:]
                    max_objective_value = current_objective_value
                    consider_objective_value[0] = max_objective_value
                    consider_quality_array[0] = temp_quality_array[:]
                    print("====== consider_quality_array: {}".format(consider_quality_array))
                    print("====== consider_objective_value: {}".format(consider_objective_value))

                new_quality_array[-1] = q_n
                # Minh - when there is no gap in the buffer, also consider objective value for the next segment - S
                if len(gap_index_array_sorted) == 1:  # no gap in the buffer
                    continue
                else:
                    # Minh - when there is no gap in the buffer, also consider objective value for the next segment - E
                    for gap_id in range(1, len(gap_index_array_sorted)):
                        print("==> Gap {}: {}".format(gap_id, gap_index_array_sorted[gap_id]))
                        # t_download = [available_bitrates[q_n]*segment_duration/estimated_throughput]

                        if retrans_strategy:  # 1 means download segments in 1 gaps only. 0 means download all gaps at once
                            # for each gap, start upgrading from the original quality_array
                            new_quality_array[:-1] = quality_array[:-1]
                            # new_quality_array.append(q_n)

                        for seg_id in reversed(gap_index_array_sorted[gap_id]):
                            print("\t==> Seg_id {}".format(seg_id))
                            t_download.append(0)

                            for retrans_quality_id in range(quality_array[seg_id] + 1,
                                                            target_quality_array_sorted[gap_id] + 1):
                                print("\t\t==> retrans_quality_id {}. bitrate {}".format(retrans_quality_id,
                                                                                         available_bitrates[
                                                                                             retrans_quality_id]))
                                t_download[-1] = t_download[-2] + available_bitrates[
                                    retrans_quality_id] * segment_duration / estimated_throughput
                                if gap_id == 0:
                                    available_time = available_times[0][0]
                                else:
                                    available_time = remaining_time_current_segment + (seg_id - 1) * segment_duration
                                print("\t\t==> t_download {} vs. avai_time {:.1f}".format(t_download, available_time))

                                temp_quality_array = new_quality_array[:]
                                temp_quality_array[seg_id] = retrans_quality_id

                                current_downloaded_data = 0
                                for old, new in zip(quality_array, temp_quality_array):
                                    if old != new:
                                        current_downloaded_data += available_bitrates[new]
                                total_download_time = current_downloaded_data * segment_duration / estimated_throughput

                                print("############ Download time of upgraded segment: {}. Available time: {}".format(t_download[-1], available_time))
                                if t_download[-1] > available_time: # or total_download_time > total_remaining_time:  # can't retransmit this segment
                                    # go to next gap
                                    print("##################################### Oh no, next gap ###########")
                                    next_gap_flag = True
                                    break
                                else:
                                    isRetransPossible = True
                                    # calculate objective function
                                    # temp_quality_array = new_quality_array[:]
                                    # temp_quality_array[seg_id] = retrans_quality_id
                                    print("\t\t==> temp_quality_array: {}".format(temp_quality_array))
                                    current_objective_value = get_objective_function(temp_quality_array)
                                    if max_objective_value < current_objective_value:
                                        print("\t\t\t-------==> objective_value: {}".format(current_objective_value))
                                        new_quality_array = temp_quality_array[:]
                                        print("\t\t\t-------==> new_quality_array: {}".format(new_quality_array))
                                        max_objective_value = current_objective_value

                                        if retrans_strategy:
                                            consider_quality_array[gap_id] = temp_quality_array[:]
                                            consider_objective_value[gap_id] = current_objective_value
                                            print("\t\t\t-------==> consider_objective_value: {}".format(
                                                consider_objective_value))
                                            print("\t\t\t-------==> consider_quality_array: {}".format(
                                                consider_quality_array))
                            if next_gap_flag:
                                next_gap_flag = False
                                break

            print("====== consider_quality_array: {}".format(consider_quality_array))
            print("====== consider_objective_value: {}".format(consider_objective_value))

            if retrans_strategy:
                print(np.argsort(consider_objective_value))
                new_quality_array = consider_quality_array[np.argsort(consider_objective_value)[-1]][:]
            print("======    quality_array: {}".format(quality_array))
            print("====== newquality_array: {}".format(new_quality_array))
            ######################################################################################
            downloaded_data = 0
            for old, new in zip(quality_array, new_quality_array):
                if old != new:
                    downloaded_data += available_bitrates[new]
            print("====== Downloaded_data: {}kbps".format(downloaded_data))

            # Retransmission segments order --> reverse lists in gap_index_array_sorted
            retransmission_queue = [j for i in gap_index_array_sorted for j in reversed(i)]
            print(retransmission_queue)
            new_quality_array.extend(retransmission_queue[1:])
            return new_quality_array


if __name__ == "__main__":
    run_dofp()

