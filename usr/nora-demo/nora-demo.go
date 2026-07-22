// nora-demo -- a small tour of Nora's Go debugger, for exploring the dashboard.
//
// Open it to read + edit + get gopls diagnostics/hover/go-to-def:
//
//     nora /goroot/demo/nora-demo.go
//
// Debug the compiled copy (needs the Ambush debugger installed) and drive the
// dashboard -- the sidebar tiles + the cross-boundary call stack:
//
//     :debug /goroot/bin/nora-demo   start (stops at entry)
//     :break main.total              break where the loop sums the prices
//     :cont                          run to it     (F5 also continues)
//     F10 / F11                      step over / into      Shift-F11 out
//     Tab                            move focus editor -> the sidebar tiles
//       in Variables:  l/h expand/collapse a struct, slice or map (items,
//                      counts); j/k select; Esc back to the editor
//       in Call Stack: j/k select a frame, Enter jumps to its source; below
//                      the ember "-- kernel --" divider are the kernel frames
//                      the goroutine is running in (the cross-boundary stack)
//       in Goroutines: the worker goroutines parked on the channel
//     :print grand                   evaluate an expression at the stop  (:p)
//     Space d                        panel toggles: v sidebar  c console  z zoom
//     :cont                          run to completion       :kill to stop early
//
// Good places to break: main.total (step + watch `sum`), main.priciest (inspect
// the winning Item struct), main.tally (inspect the map), main.worker (a
// goroutine parked on a channel). Every one of these has a loop, so the compiler
// keeps it as a real, breakpoint-able function.
package main

import (
	"fmt"
	"sort"
	"sync"
)

// Item is a thing with a price and some tags -- a small struct to inspect in the
// Variables tile (expand it with `l` to see the fields, then the Tags slice).
type Item struct {
	Name  string
	Price int
	Tags  []string
}

// total sums the prices. Break here, then F10 to watch `sum` grow each pass.
func total(items []Item) int {
	sum := 0
	for _, it := range items {
		sum += it.Price
	}
	return sum
}

// priciest returns the most expensive item. Break here to expand the `best`
// Item struct (and `it` as the loop walks) in the Variables tile.
func priciest(items []Item) Item {
	best := items[0]
	for _, it := range items[1:] {
		if it.Price > best.Price {
			best = it
		}
	}
	return best
}

// tally counts how often each tag appears -- a map[string]int to inspect.
func tally(items []Item) map[string]int {
	counts := make(map[string]int)
	for _, it := range items {
		for _, tag := range it.Tags {
			counts[tag]++
		}
	}
	return counts
}

// worker multiplies each item's price by its worker id. It ranges over a
// channel, so a stopped worker is parked in a channel receive -- a goroutine to
// see in the Goroutines tile, with real kernel frames under the divider.
func worker(id int, in <-chan Item, out chan<- int, wg *sync.WaitGroup) {
	defer wg.Done()
	for it := range in {
		out <- it.Price * id
	}
}

func main() {
	items := []Item{
		{Name: "quill", Price: 12, Tags: []string{"write", "bird"}},
		{Name: "ink", Price: 5, Tags: []string{"write"}},
		{Name: "lantern", Price: 30, Tags: []string{"light"}},
		{Name: "map", Price: 18, Tags: []string{"paper", "travel"}},
	}

	// Sort cheapest-first, so the slice you inspect has a known order.
	sort.Slice(items, func(i, j int) bool { return items[i].Price < items[j].Price })

	fmt.Println("total:", total(items))
	fmt.Println("priciest:", priciest(items).Name)

	counts := tally(items)
	fmt.Println("distinct tags:", len(counts))

	// A little concurrency: three workers priced by a fan-in over channels, so
	// the Goroutines tile + the "-- kernel --" divider have something to show.
	in := make(chan Item)
	out := make(chan int)
	var wg sync.WaitGroup
	for id := 1; id <= 3; id++ {
		wg.Add(1)
		go worker(id, in, out, &wg)
	}
	go func() {
		for _, it := range items {
			in <- it
		}
		close(in)
	}()
	go func() {
		wg.Wait()
		close(out)
	}()

	grand := 0
	for v := range out {
		grand += v
	}
	fmt.Println("grand:", grand)
}
